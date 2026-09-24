#pragma once

#include <atomic>
#include <cstdint>

#include "ruvia/core/WorkerHandle.h"

namespace ruvia {

// Thread-safe counters for outcomes of worker-post attempts.
class WorkerPostCounters {
public:
    void record(PostStatus status) noexcept {
        switch (status) {
            case PostStatus::kAccepted:
                accepted_.fetch_add(1, std::memory_order_relaxed);
                break;
            case PostStatus::kQueueFull:
                queueFull_.fetch_add(1, std::memory_order_relaxed);
                break;
            case PostStatus::kWorkerStopping:
                workerStopping_.fetch_add(1, std::memory_order_relaxed);
                break;
        }
    }

    void recordWorkerStopping() noexcept {
        workerStopping_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t accepted() const noexcept {
        return accepted_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t queueFull() const noexcept {
        return queueFull_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t workerStopping() const noexcept {
        return workerStopping_.load(std::memory_order_relaxed);
    }

private:
    std::atomic_uint64_t accepted_{0};
    std::atomic_uint64_t queueFull_{0};
    std::atomic_uint64_t workerStopping_{0};
};

}  // namespace ruvia
