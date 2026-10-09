#pragma once

#include <atomic>
#include <cstdint>

#include "ruvia/core/worker_handle.h"

namespace ruvia {

// Thread-safe counters for outcomes of worker-post attempts.
class worker_post_counters {
public:
    void record(post_status status) noexcept {
        switch (status) {
            case post_status::accepted:
                accepted_.fetch_add(1, std::memory_order_relaxed);
                break;
            case post_status::queue_full:
                queue_full_.fetch_add(1, std::memory_order_relaxed);
                break;
            case post_status::worker_stopping:
                worker_stopping_.fetch_add(1, std::memory_order_relaxed);
                break;
        }
    }

    void record_worker_stopping() noexcept {
        worker_stopping_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t accepted() const noexcept {
        return accepted_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t queue_full() const noexcept {
        return queue_full_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t worker_stopping() const noexcept {
        return worker_stopping_.load(std::memory_order_relaxed);
    }

private:
    std::atomic_uint64_t accepted_{0};
    std::atomic_uint64_t queue_full_{0};
    std::atomic_uint64_t worker_stopping_{0};
};

}  // namespace ruvia
