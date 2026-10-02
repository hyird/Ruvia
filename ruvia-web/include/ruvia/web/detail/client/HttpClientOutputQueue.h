#pragma once

#include <memory_resource>
#include <string>

#include "ruvia/core/ScopedOperation.h"
#include "ruvia/core/WorkerSignal.h"

namespace ruvia::detail {
// Owned by the response memory domain, not by a connection/session arena.
// At most one application chunk is queued; wire drivers borrow it until their
// protocol cursor commits it. Terminal wakeups never free a pending SSL span.
class HttpClientOutputQueue {
public:
    HttpClientOutputQueue(const WorkerHandle& worker, std::pmr::memory_resource* resource)
        : data(worker),
          space(worker),
          chunk(resource) {}
    void notifyData() noexcept {
        data.notify();
        if (wake != nullptr) {
            wake(wakeTarget);
        }
    }
    void acknowledgeChunk() noexcept {
        chunkReady = false;
        std::pmr::string(chunk.get_allocator()).swap(chunk);
        space.notify();
    }
    void finish() noexcept {
        ended = true;
        space.notify();
    }
    void stop() noexcept {
        if (stopped) {
            return;
        }
        stopped = true;
        data.notify();
        space.notify();
        if (wake != nullptr) {
            wake(wakeTarget);
        }
    }
    WorkerSignal data;
    WorkerSignal space;
    std::pmr::string chunk;
    bool chunkReady{};
    bool endRequested{};
    bool completionPending{};
    bool ended{};
    bool stopped{};
    void* wakeTarget{};
    void (*wake)(void*) noexcept {};
    ScopedOperationScope outputScope;
};
}  // namespace ruvia::detail
