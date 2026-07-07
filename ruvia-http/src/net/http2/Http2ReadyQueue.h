#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Http2FrameTypes.h"

namespace ruvia::detail {

class Http2ReadyQueue final {
public:
    [[nodiscard]] bool push(std::uint32_t streamId) noexcept {
        if (size_ >= streamIds_.size() && offset_ > 0) {
            // The array is physically full but the consumed prefix is reclaimable.
            // pop() compacts only lazily, so after filling to capacity and popping
            // a few entries the buffer can stay full while holding fewer than
            // capacity live streams. Reclaim the prefix here so a ready stream is
            // never spuriously rejected (which would stall it) while the queue is
            // not logically full.
            const auto remaining = size_ - offset_;
            std::memmove(
                streamIds_.data(),
                streamIds_.data() + offset_,
                remaining * sizeof(std::uint32_t));
            offset_ = 0;
            size_ = remaining;
        }
        if (size_ >= streamIds_.size()) {
            return false;
        }
        streamIds_[size_++] = streamId;
        return true;
    }

    [[nodiscard]] bool hasReady() const noexcept {
        return offset_ < size_;
    }

    [[nodiscard]] std::uint32_t pop() noexcept {
        const auto streamId = streamIds_[offset_++];
        compact();
        return streamId;
    }

    void remove(std::uint32_t streamId) noexcept {
        std::size_t write = 0;
        for (std::size_t read = offset_; read < size_; ++read) {
            if (streamIds_[read] != streamId) {
                streamIds_[write++] = streamIds_[read];
            }
        }
        offset_ = 0;
        size_ = write;
    }

private:
    void compact() noexcept {
        if (offset_ == 0) {
            return;
        }
        if (offset_ == size_) {
            offset_ = 0;
            size_ = 0;
            return;
        }
        if (offset_ < 64 && offset_ < size_ - offset_) {
            return;
        }
        const auto remaining = size_ - offset_;
        std::memmove(streamIds_.data(), streamIds_.data() + offset_, remaining * sizeof(std::uint32_t));
        offset_ = 0;
        size_ = remaining;
    }

    std::array<std::uint32_t, kHttp2LocalMaxConcurrentStreams> streamIds_{};
    std::size_t size_{0};
    std::size_t offset_{0};
};

}  // namespace ruvia::detail
