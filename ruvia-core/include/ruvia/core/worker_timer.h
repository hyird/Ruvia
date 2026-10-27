#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ratio>
#include <type_traits>
#include <utility>

namespace ruvia::detail {
class worker_dispatcher;
struct timer_slot;
}  // namespace ruvia::detail

namespace ruvia {

class worker_timer_cancellation final {
public:
    worker_timer_cancellation() noexcept = default;

    void cancel() const noexcept;
    [[nodiscard]] bool valid() const noexcept {
        return dispatcher_ != nullptr && generation_ != 0;
    }

private:
    // Borrows the dispatcher kept alive by the awaiter's stable worker_handle.
    // Stop callback teardown synchronizes the borrow; cancellation dispatch owns
    // any continuation it posts.
    worker_timer_cancellation(
        detail::worker_dispatcher& dispatcher, std::size_t slot, std::uint64_t generation) noexcept
        : dispatcher_(&dispatcher),
          slot_(slot),
          generation_(generation) {}

    detail::worker_dispatcher* dispatcher_{nullptr};
    std::size_t slot_{0};
    std::uint64_t generation_{0};

    friend class worker_timer_registration;
};

enum class worker_timer_outcome : std::uint8_t {
    expired,
    // cancel_and_notify(), a worker_timer_cancellation, or worker shutdown
    // (stop_timers) ended the pending timer.
    cancelled,
};

// Result of worker_handle::schedule_timer(). Shutdown is a normal state, not a
// failure: once the worker's timer queue has stopped, scheduling returns
// worker_stopping, stores nothing, never invokes the completion, and leaves the
// registration unbound. That is the same terminal point a pending timer reaches
// through stop_timers(), which delivers worker_timer_outcome::cancelled; timed
// primitives map both to their own shutdown result.
// This also applies when moving the completion itself stops the timer queue.
enum class worker_timer_schedule_status : std::uint8_t {
    scheduled,
    worker_stopping,
};

// Converts to steady-clock ticks with saturation at both bounds. Integral
// whole-factor scaling/division of counts up to uintmax_t width stays exact.
// Other conversions use long double; NaN maps to zero, infinities saturate.
template <typename rep_type, typename period_type>
[[nodiscard]] inline std::chrono::steady_clock::duration worker_timer_saturating_duration_cast(
    std::chrono::duration<rep_type, period_type> value) {
    using target_type = std::chrono::steady_clock::duration;
    using factor = std::ratio_divide<period_type, typename target_type::period>;
    if constexpr (std::is_integral_v<rep_type> &&
                  std::numeric_limits<rep_type>::digits <= std::numeric_limits<std::uintmax_t>::digits &&
                  (factor::num == 1 || factor::den == 1)) {
        using count_type = std::conditional_t<std::is_signed_v<rep_type>, std::intmax_t, std::uintmax_t>;
        const auto count = static_cast<count_type>(value.count()) / factor::den;
        if (std::cmp_greater(count, target_type::max().count() / factor::num)) {
            return target_type::max();
        }
        if (std::cmp_less(count, target_type::min().count() / factor::num)) {
            return target_type::min();
        }
        return target_type(static_cast<typename target_type::rep>(count) * factor::num);
    } else {
        // Discard this alternative for whole-factor integer conversions so
        // MSVC /WX does not reject an emitted unreachable fallback.
        using wide_type = std::chrono::duration<long double, typename target_type::period>;
        const auto count = std::chrono::duration_cast<wide_type>(value).count();
        if (std::isnan(count)) {
            return target_type::zero();
        }
        const auto maximum = static_cast<long double>(target_type::max().count());
        if (count >= maximum) {
            return target_type::max();
        }
        const auto minimum = static_cast<long double>(target_type::min().count());
        if (count <= minimum) {
            return target_type::min();
        }
        return target_type(static_cast<typename target_type::rep>(count));
    }
}

[[nodiscard]] inline std::chrono::steady_clock::time_point worker_timer_saturating_deadline(
    std::chrono::steady_clock::time_point now, std::chrono::steady_clock::duration delay) noexcept {
    if (delay <= std::chrono::steady_clock::duration::zero()) {
        return now;
    }
    constexpr auto maximum = std::chrono::steady_clock::time_point::max();
    if (now > maximum - delay) {
        return maximum;
    }
    return now + delay;
}

[[nodiscard]] inline std::chrono::steady_clock::time_point worker_timer_deadline_after(
    std::chrono::steady_clock::duration delay) noexcept {
    return worker_timer_saturating_deadline(std::chrono::steady_clock::now(), delay);
}

// Non-positive delays map to zero; positive delays round up to whole
// milliseconds and saturate at the representable maximum.
[[nodiscard]] inline std::chrono::milliseconds worker_timer_ceil_milliseconds(
    std::chrono::steady_clock::duration value) noexcept {
    using milliseconds_type = std::chrono::milliseconds;
    if (value <= std::chrono::steady_clock::duration::zero()) {
        return milliseconds_type::zero();
    }

    using factor = std::ratio_divide<std::chrono::steady_clock::duration::period, milliseconds_type::period>;
    if constexpr (factor::num == 1) {
        const auto count = value.count() / factor::den;
        if (std::cmp_greater_equal(count, milliseconds_type::max().count())) {
            return milliseconds_type::max();
        }
        return milliseconds_type(static_cast<milliseconds_type::rep>(count) + (value.count() % factor::den != 0));
    } else if constexpr (factor::den == 1) {
        if (std::cmp_greater(value.count(), milliseconds_type::max().count() / factor::num)) {
            return milliseconds_type::max();
        }
        return milliseconds_type(static_cast<milliseconds_type::rep>(value.count()) * factor::num);
    } else {
        using millisecond_float_type = std::chrono::duration<long double, milliseconds_type::period>;
        const auto count = std::chrono::duration_cast<millisecond_float_type>(value).count();
        if (std::isnan(count)) {
            return milliseconds_type::zero();
        }
        const auto maximum = static_cast<long double>(milliseconds_type::max().count());
        const auto rounded = std::ceil(count);
        if (rounded >= maximum) {
            return milliseconds_type::max();
        }
        return milliseconds_type(static_cast<milliseconds_type::rep>(rounded));
    }
}

template <typename rep_type, typename period_type>
[[nodiscard]] inline std::chrono::steady_clock::time_point worker_timer_deadline_after(
    std::chrono::duration<rep_type, period_type> delay) {
    return worker_timer_saturating_deadline(
        std::chrono::steady_clock::now(), worker_timer_saturating_duration_cast(delay));
}

// Borrowed timer token. A registration is bound to the worker of its most
// recent schedule_timer() until that timer expires, is cancelled, or is
// released by cancel()/cancel_and_notify(). Rescheduling while registration
// is in progress or the timer is still pending throws std::logic_error. Once
// it has expired or been cancelled, it may be scheduled on another worker;
// that worker observes the release with acquire/release ordering, so a caller
// that learned of the expiry through its completion may reuse it.
class worker_timer_registration final {
public:
    worker_timer_registration() noexcept = default;
    ~worker_timer_registration();

    worker_timer_registration(const worker_timer_registration&) = delete;
    worker_timer_registration& operator=(const worker_timer_registration&) = delete;
    worker_timer_registration(worker_timer_registration&&) = delete;
    worker_timer_registration& operator=(worker_timer_registration&&) = delete;

    // Removes the registration without delivering its completion: the owner
    // that cancels has stopped observing the outcome (a finished operation,
    // a re-arm, setup rollback). Destruction does the same.
    void cancel() noexcept;
    // Removes the registration and delivers worker_timer_outcome::cancelled,
    // for a completion that must run to resume or release its waiter. On the
    // worker's own thread the completion is posted rather than re-entered.
    void cancel_and_notify() noexcept;
    [[nodiscard]] worker_timer_cancellation cancellation() const&;
    worker_timer_cancellation cancellation() const&& = delete;
    // Expiry consumes the queue entry without writing through this borrowed
    // registration; its stable owner releases or reuses the token afterwards.
    [[nodiscard]] bool registered() const noexcept;

private:
    void unregister(bool notify) noexcept;
    void bind(detail::worker_dispatcher& dispatcher, std::size_t slot, std::uint64_t generation,
        const detail::timer_slot& slot_state) noexcept;
    void release() noexcept;
    [[nodiscard]] bool pending() const noexcept;

    // The handle supplied to schedule_timer() must outlive this registration.
    // Destruction only removes it; it never queues a callback referencing the
    // destroyed owner.
    detail::worker_dispatcher* dispatcher_{nullptr};
    // Owned by that dispatcher; its activity is readable from any thread.
    const detail::timer_slot* slot_state_{nullptr};
    std::size_t slot_{0};
    std::uint64_t generation_{0};
    bool scheduling_{false};

    friend class detail::worker_dispatcher;
};

}  // namespace ruvia
