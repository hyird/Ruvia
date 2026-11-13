#pragma once

#include <bit>
#include <chrono>
#include <cmath>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ratio>
#include <type_traits>
#include <utility>

namespace ruvia {

// Converts directly from the original count domain to the requested tick type.
// Integral counts up to uintmax_t width stay exact, rounding positive fractions
// upward and saturating at both bounds. Other conversions use long double;
// NaN maps to zero, infinities saturate.
template <typename target_type = std::chrono::steady_clock::duration, typename rep_type, typename period_type>
[[nodiscard]] constexpr target_type saturating_duration_cast(
    std::chrono::duration<rep_type, period_type> value) noexcept {
    static_assert(std::is_integral_v<typename target_type::rep> && std::is_signed_v<typename target_type::rep>);
    using factor = std::ratio_divide<period_type, typename target_type::period>;
    if constexpr (std::is_integral_v<rep_type> &&
                  std::numeric_limits<rep_type>::digits <= std::numeric_limits<std::uintmax_t>::digits &&
                  (factor::num == 1 || factor::den == 1)) {
        using count_type = std::conditional_t<std::is_signed_v<rep_type>, std::intmax_t, std::uintmax_t>;
        const auto raw = static_cast<count_type>(value.count());
        auto count = raw / factor::den;
        if constexpr (factor::den > 1) {
            if (raw > 0 && raw % factor::den != 0) {
                if (std::cmp_greater_equal(count, target_type::max().count() / factor::num)) {
                    return target_type::max();
                }
                ++count;
            }
        }
        if (std::cmp_greater(count, target_type::max().count() / factor::num)) {
            return target_type::max();
        }
        if (std::cmp_less(count, target_type::min().count() / factor::num)) {
            return target_type::min();
        }
        return target_type(static_cast<typename target_type::rep>(count) * factor::num);
    } else if constexpr (std::is_integral_v<rep_type> &&
                         std::numeric_limits<rep_type>::digits <= std::numeric_limits<std::uintmax_t>::digits) {
        using count_type = std::conditional_t<std::is_signed_v<rep_type>, std::intmax_t, std::uintmax_t>;
        using target_rep = typename target_type::rep;
        const bool negative = std::cmp_less(static_cast<count_type>(value.count()), 0);
        const auto raw = static_cast<std::uintmax_t>(value.count());
        const auto magnitude = negative ? std::uintmax_t{0} - raw : raw;
        const auto limit = negative
                               ? std::uintmax_t{0} - static_cast<std::uintmax_t>(target_type::min().count())
                               : static_cast<std::uintmax_t>(target_type::max().count());
        constexpr auto numerator = static_cast<std::uintmax_t>(factor::num);
        constexpr auto denominator = static_cast<std::uintmax_t>(factor::den);
        const auto whole = magnitude / denominator;
        if (whole > limit / numerator) {
            return negative ? target_type::min() : target_type::max();
        }
        const auto integral = whole * numerator;
        const auto remainder = magnitude % denominator;
        std::uintmax_t fraction = 0;
        std::uintmax_t residue = 0;
        if (remainder <= (std::numeric_limits<std::uintmax_t>::max)() / numerator) {
            const auto product = remainder * numerator;
            fraction = product / denominator;
            residue = product % denominator;
        } else {
            // Divide the remainder product bitwise instead of overflowing it.
            // Each residue addition is below 2 * denominator <= UINTMAX_MAX.
            for (auto bit = std::bit_floor(numerator); bit != 0; bit >>= 1) {
                fraction *= 2;
                residue *= 2;
                if (residue >= denominator) {
                    residue -= denominator;
                    ++fraction;
                }
                if ((numerator & bit) != 0) {
                    residue += remainder;
                    if (residue >= denominator) {
                        residue -= denominator;
                        ++fraction;
                    }
                }
            }
        }
        if (!negative && residue != 0) {
            ++fraction;
        }
        if (fraction >= limit - integral) {
            return negative ? target_type::min() : target_type::max();
        }
        const auto ticks = static_cast<target_rep>(integral + fraction);
        return target_type(negative ? -ticks : ticks);
    } else {
        // Discard this alternative for integral conversions so MSVC /WX does
        // not reject an emitted unreachable fallback.
        using wide_type = std::chrono::duration<long double, typename target_type::period>;
        const auto count = std::chrono::duration_cast<wide_type>(value).count();
        if (std::isnan(count)) {
            return target_type::zero();
        }
        // Unit conversion may underflow before ceil sees the positive input.
        if (count == 0.0L && value.count() > 0) {
            return target_type(1);
        }
        const auto rounded = count > 0.0L ? std::ceil(count) : std::trunc(count);
        const auto maximum = static_cast<long double>(target_type::max().count());
        if (rounded >= maximum) {
            return target_type::max();
        }
        const auto minimum = static_cast<long double>(target_type::min().count());
        if (rounded <= minimum) {
            return target_type::min();
        }
        return target_type(static_cast<typename target_type::rep>(rounded));
    }
}

// Public relative-time inputs retain their original representation until this
// boundary. Non-positive values remain available to each consumer's policy.
class timeout_duration final {
public:
    constexpr timeout_duration() noexcept = default;

    template <std::integral rep_type, typename period_type>
    constexpr timeout_duration(std::chrono::duration<rep_type, period_type> value) noexcept
        : value_(saturating_duration_cast<std::chrono::milliseconds>(value)) {}

    template <std::integral count_type>
    explicit constexpr timeout_duration(count_type milliseconds) noexcept
        : timeout_duration(std::chrono::duration<count_type, std::milli>(milliseconds)) {}

    [[nodiscard]] constexpr std::chrono::milliseconds value() const noexcept {
        return value_;
    }

    // Mixed comparisons must unwrap explicitly: rounding elapsed time here
    // could otherwise make a positive fractional millisecond expire early.
    template <typename left_type, typename right_type>
        requires std::same_as<std::remove_cvref_t<left_type>, timeout_duration> &&
                 std::same_as<std::remove_cvref_t<right_type>, timeout_duration>
    friend constexpr auto operator<=>(left_type&& left, right_type&& right) noexcept {
        return left.value_ <=> right.value_;
    }

    template <typename left_type, typename right_type>
        requires std::same_as<std::remove_cvref_t<left_type>, timeout_duration> &&
                 std::same_as<std::remove_cvref_t<right_type>, timeout_duration>
    friend constexpr bool operator==(left_type&& left, right_type&& right) noexcept {
        return left.value_ == right.value_;
    }

private:
    std::chrono::milliseconds value_{};
};

template <typename rep_type, typename period_type>
[[nodiscard]] inline std::chrono::steady_clock::time_point saturating_deadline(
    std::chrono::steady_clock::time_point now, std::chrono::duration<rep_type, period_type> value) noexcept {
    const auto delay = saturating_duration_cast(value);
    if (delay <= std::chrono::steady_clock::duration::zero()) {
        return now;
    }
    constexpr auto maximum = std::chrono::steady_clock::time_point::max();
    if (now > maximum - delay) {
        return maximum;
    }
    return now + delay;
}

// Non-positive delays map to zero; positive delays round up to whole
// milliseconds and saturate at the representable maximum.
template <typename rep_type, typename period_type>
[[nodiscard]] constexpr std::chrono::milliseconds ceil_milliseconds(
    std::chrono::duration<rep_type, period_type> value) noexcept {
    if (value <= std::chrono::duration<rep_type, period_type>::zero()) {
        return std::chrono::milliseconds::zero();
    }
    return saturating_duration_cast<std::chrono::milliseconds>(value);
}

template <typename rep_type, typename period_type>
[[nodiscard]] inline std::chrono::steady_clock::time_point deadline_after(
    std::chrono::duration<rep_type, period_type> delay) noexcept {
    return saturating_deadline(std::chrono::steady_clock::now(), delay);
}

}  // namespace ruvia

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
    // Borrows the dispatcher kept alive by the awaiter's stable worker_ref.
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

// Result of worker_ref::schedule_timer(). Shutdown is a normal state, not a
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

    // The worker_ref supplied to schedule_timer() must outlive this registration.
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
