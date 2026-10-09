#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ruvia::detail {
class worker_dispatcher;
}

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
    cancelled,
};

template <typename rep_type, typename period_type>
[[nodiscard]] inline std::chrono::steady_clock::duration worker_timer_saturating_duration_cast(
    std::chrono::duration<rep_type, period_type> value) {
    using target_type = std::chrono::steady_clock::duration;
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

[[nodiscard]] inline std::chrono::milliseconds worker_timer_ceil_milliseconds(
    std::chrono::steady_clock::duration value) noexcept {
    using milliseconds_type = std::chrono::milliseconds;
    if (value <= std::chrono::steady_clock::duration::zero()) {
        return milliseconds_type::zero();
    }

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

template <typename rep_type, typename period_type>
[[nodiscard]] inline std::chrono::steady_clock::time_point worker_timer_deadline_after(
    std::chrono::duration<rep_type, period_type> delay) {
    return worker_timer_saturating_deadline(
        std::chrono::steady_clock::now(), worker_timer_saturating_duration_cast(delay));
}

class worker_timer_registration final {
public:
    worker_timer_registration() noexcept = default;
    ~worker_timer_registration();

    worker_timer_registration(const worker_timer_registration&) = delete;
    worker_timer_registration& operator=(const worker_timer_registration&) = delete;
    worker_timer_registration(worker_timer_registration&&) = delete;
    worker_timer_registration& operator=(worker_timer_registration&&) = delete;

    void cancel() noexcept;
    // Removes the registration without delivering completion; used only while
    // rolling back setup before publishing an awaiter continuation.
    void cancel_quietly() noexcept;
    [[nodiscard]] worker_timer_cancellation cancellation() const&;
    worker_timer_cancellation cancellation() const&& = delete;
    // Expiry consumes the queue entry without writing through this borrowed
    // registration; its stable owner releases or reuses the token afterwards.
    [[nodiscard]] bool registered() const noexcept;

private:
    void cancel(bool notify) noexcept;
    void bind(detail::worker_dispatcher& dispatcher, std::size_t slot, std::uint64_t generation) noexcept;
    void release() noexcept;

    // The handle supplied to schedule_timer() must outlive this registration.
    // Destruction only removes it; it never queues a callback referencing the
    // destroyed owner.
    detail::worker_dispatcher* dispatcher_{nullptr};
    std::size_t slot_{0};
    std::uint64_t generation_{0};

    friend class detail::worker_dispatcher;
};

}  // namespace ruvia
