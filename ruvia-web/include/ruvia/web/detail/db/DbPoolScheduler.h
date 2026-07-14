#pragma once

#include "ruvia/core/Task.h"
#include "ruvia/core/detail/PoolWaiterQueue.h"
#include "ruvia/core/memory/PmrResource.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <vector>

namespace ruvia::detail {

// Per-worker, allocation-stable pool lease scheduler shared by every database
// driver. The scheduler owns queueing and acquire deadlines; concrete drivers
// own only their protocol connection slots.
class DbPoolScheduler final {
public:
    DbPoolScheduler(
        std::size_t poolSize,
        std::pmr::memory_resource* resource = nullptr);

    DbPoolScheduler(const DbPoolScheduler&) = delete;
    DbPoolScheduler& operator=(const DbPoolScheduler&) = delete;

    [[nodiscard]] Task<std::size_t> acquire(
        std::optional<std::chrono::milliseconds> timeout);
    void release(std::size_t slot) noexcept;
    [[nodiscard]] bool close() noexcept;
    void scanDeadlines(std::chrono::steady_clock::time_point now) noexcept;
    [[nodiscard]] bool closing() const noexcept;

private:
    std::pmr::vector<std::size_t> freeSlots_;
    std::pmr::vector<std::uint8_t> busy_;
    PoolWaiterQueue waiters_;
    bool closing_{false};
};

}  // namespace ruvia::detail
