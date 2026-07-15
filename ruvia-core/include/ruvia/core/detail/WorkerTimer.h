#pragma once

#include <cstdint>
#include <memory>

namespace ruvia::detail {

class WorkerDispatcher;
struct WorkerTimerState;

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
    WorkerTimerRegistration(WorkerTimerRegistration&& other) noexcept;
    WorkerTimerRegistration& operator=(WorkerTimerRegistration&& other) noexcept;

    void cancel() noexcept;
    [[nodiscard]] bool valid() const noexcept;

private:
    WorkerTimerRegistration(std::shared_ptr<WorkerDispatcher> dispatcher,
                            std::shared_ptr<WorkerTimerState> state) noexcept;

    std::shared_ptr<WorkerDispatcher> dispatcher_;
    std::shared_ptr<WorkerTimerState> state_;

    friend class WorkerDispatcher;
};

}
