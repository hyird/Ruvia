#pragma once

#include <coroutine>
#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

namespace ruvia::detail {

class Http2StreamBodyQueue final {
public:
    explicit Http2StreamBodyQueue(std::pmr::memory_resource* resource)
        : queuedChunk_(resource),
          activeChunk_(resource),
          overflowChunks_(resource) {}

    [[nodiscard]] bool hasOverflowQueuedChunk() const noexcept;
    void enqueue(std::string_view data);
    void enqueueOwned(std::pmr::string& body);
    [[nodiscard]] bool hasQueuedChunk() const noexcept;
    void compact();
    [[nodiscard]] std::string_view pop();
    void setWaiter(std::coroutine_handle<> continuation) noexcept;
    [[nodiscard]] std::coroutine_handle<> takeWaiter() noexcept;

private:
    std::pmr::string queuedChunk_;
    std::pmr::string activeChunk_;
    std::pmr::vector<std::pmr::string> overflowChunks_;
    std::size_t overflowChunkOffset_{0};
    bool hasQueuedChunk_ : 1 {false};
    std::coroutine_handle<> waiter_{};
};

}  // namespace ruvia::detail
