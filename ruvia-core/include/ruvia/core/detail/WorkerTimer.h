#pragma once

#include <cstddef>
#include <cstdint>

namespace ruvia::detail {

class WorkerDispatcher;

enum class WorkerTimerOutcome : std::uint8_t {
    kExpired,
    kCancelled,
};

class WorkerTimerRegistration final {
public:
    WorkerTimerRegistration() noexcept = default;
    ~WorkerTimerRegistration();

    WorkerTimerRegistration(const WorkerTimerRegistration&) = delete;
    WorkerTimerRegistration& operator=(const WorkerTimerRegistration&) = delete;
    WorkerTimerRegistration(WorkerTimerRegistration&&) = delete;
    WorkerTimerRegistration& operator=(WorkerTimerRegistration&&) = delete;

    void cancel() noexcept;
    // Whether this registration still owns a cancellation token. Expiry consumes
    // the queue entry but deliberately does not write back through this borrowed
    // registration; its stable owner releases or reuses the token afterwards.
    [[nodiscard]] bool registered() const noexcept;

private:
    void bind(WorkerDispatcher& dispatcher,
              std::size_t slot,
              std::uint64_t generation) noexcept;
    void release() noexcept;

    // The handle supplied to scheduleTimer() must outlive this registration.
    // Internal users enforce that structurally through member declaration order.
    WorkerDispatcher* dispatcher_{nullptr};
    std::size_t slot_{0};
    std::uint64_t generation_{0};

    friend class WorkerDispatcher;
};

}
