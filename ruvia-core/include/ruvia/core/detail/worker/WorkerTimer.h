#pragma once

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ruvia::detail {

class WorkerDispatcher;

class WorkerTimerCancellation final {
public:
    WorkerTimerCancellation() noexcept = default;

    void cancel() const noexcept;
    [[nodiscard]] bool valid() const noexcept {
        return dispatcher_ != nullptr && generation_ != 0;
    }

private:
    // Borrows the dispatcher kept alive by the awaiter's stable WorkerHandle.
    // StopRegistration teardown synchronizes any callback before that borrow
    // ends; requestTimerCancellation() owns any continuation it posts.
    WorkerTimerCancellation(
        WorkerDispatcher& dispatcher, std::size_t slot, std::uint64_t generation) noexcept
        : dispatcher_(&dispatcher),
          slot_(slot),
          generation_(generation) {}

    WorkerDispatcher* dispatcher_{nullptr};
    std::size_t slot_{0};
    std::uint64_t generation_{0};

    friend class WorkerTimerRegistration;
};

enum class WorkerTimerOutcome : std::uint8_t {
    kExpired,
    kCancelled,
};

template <typename Rep, typename Period>
[[nodiscard]] inline std::chrono::steady_clock::duration workerTimerSaturatingDurationCast(
    std::chrono::duration<Rep, Period> value) {
    using Target = std::chrono::steady_clock::duration;
    using Wide = std::chrono::duration<long double, typename Target::period>;
    const auto count = std::chrono::duration_cast<Wide>(value).count();
    if (std::isnan(count)) {
        return Target::zero();
    }
    const auto maximum = static_cast<long double>(Target::max().count());
    if (count >= maximum) {
        return Target::max();
    }
    const auto minimum = static_cast<long double>(Target::min().count());
    if (count <= minimum) {
        return Target::min();
    }
    return Target(static_cast<typename Target::rep>(count));
}

[[nodiscard]] inline std::chrono::steady_clock::time_point workerTimerSaturatingDeadline(
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

[[nodiscard]] inline std::chrono::steady_clock::time_point workerTimerDeadlineAfter(
    std::chrono::steady_clock::duration delay) noexcept {
    return workerTimerSaturatingDeadline(std::chrono::steady_clock::now(), delay);
}

template <typename Rep, typename Period>
[[nodiscard]] inline std::chrono::steady_clock::time_point workerTimerDeadlineAfter(
    std::chrono::duration<Rep, Period> delay) {
    return workerTimerSaturatingDeadline(
        std::chrono::steady_clock::now(), workerTimerSaturatingDurationCast(delay));
}

class WorkerTimerRegistration final {
public:
    WorkerTimerRegistration() noexcept = default;
    ~WorkerTimerRegistration();

    WorkerTimerRegistration(const WorkerTimerRegistration&) = delete;
    WorkerTimerRegistration& operator=(const WorkerTimerRegistration&) = delete;
    WorkerTimerRegistration(WorkerTimerRegistration&&) = delete;
    WorkerTimerRegistration& operator=(WorkerTimerRegistration&&) = delete;

    void cancel() noexcept;
    // Removes the registration without delivering a completion. This is only
    // for rolling back setup before the awaiter has published a continuation.
    void cancelQuietly() noexcept;
    [[nodiscard]] WorkerTimerCancellation cancellation() const&;
    WorkerTimerCancellation cancellation() const&& = delete;
    // Whether this registration still owns a cancellation token. Expiry consumes
    // the queue entry but deliberately does not write back through this borrowed
    // registration; its stable owner releases or reuses the token afterwards.
    [[nodiscard]] bool registered() const noexcept;

private:
    void cancel(bool notify) noexcept;
    void bind(WorkerDispatcher& dispatcher, std::size_t slot, std::uint64_t generation) noexcept;
    void release() noexcept;

    // The handle supplied to scheduleTimer() must outlive this registration.
    // Explicit cancel() delivers kCancelled to the completion. Destruction only
    // removes the registration: a callback that refers to the destroyed owner
    // must never be queued merely because its RAII token went out of scope.
    WorkerDispatcher* dispatcher_{nullptr};
    std::size_t slot_{0};
    std::uint64_t generation_{0};

    friend class WorkerDispatcher;
};

}  // namespace ruvia::detail
