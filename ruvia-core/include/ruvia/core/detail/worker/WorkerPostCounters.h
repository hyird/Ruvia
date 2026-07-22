#pragma once

#include <atomic>
#include <cstdint>

#include "ruvia/core/WorkerHandle.h"

// How a submission point tallies what happened to each post attempt. Every
// caller of WorkerHandleAccess::postFactory has to answer the same question --
// was it accepted, refused for a full queue, or refused because the worker is
// stopping -- and report the same three numbers, so the rule lives once.

namespace ruvia::detail {

class WorkerPostCounters final {
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

    // A submission refused before it reached the worker at all: the endpoint
    // had already stopped accepting.
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

}  // namespace ruvia::detail
