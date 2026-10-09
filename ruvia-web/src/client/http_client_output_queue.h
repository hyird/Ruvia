#pragma once

#include <memory_resource>
#include <string>

#include "ruvia/core/scoped_operation.h"
#include "ruvia/core/worker_signal.h"

namespace ruvia::detail {
// Owned by the response memory domain, not by a connection/session arena.
// At most one application chunk is queued; wire drivers borrow it until their
// protocol cursor commits it. Terminal wakeups never free a pending SSL span.
class http_client_output_queue final {
public:
    http_client_output_queue(const worker_handle& worker_value, std::pmr::memory_resource* resource)
        : data_(worker_value),
          space_(worker_value),
          chunk_(resource) {}
    void notify_data() noexcept {
        data_.notify();
        if (wake_ != nullptr) {
            wake_(wake_target_);
        }
    }
    void acknowledge_chunk() noexcept {
        chunk_ready_ = false;
        std::pmr::string(chunk_.get_allocator()).swap(chunk_);
        space_.notify();
    }
    void finish() noexcept {
        ended_ = true;
        space_.notify();
    }
    void stop() noexcept {
        if (stopped_) {
            return;
        }
        stopped_ = true;
        data_.notify();
        space_.notify();
        if (wake_ != nullptr) {
            wake_(wake_target_);
        }
    }
    worker_signal data_;
    worker_signal space_;
    std::pmr::string chunk_;
    bool chunk_ready_{};
    bool end_requested_{};
    bool completion_pending_{};
    bool ended_{};
    bool stopped_{};
    void* wake_target_{};
    void (*wake_)(void*) noexcept {};
    ::ruvia::operation_scope output_scope_;
};
}  // namespace ruvia::detail
