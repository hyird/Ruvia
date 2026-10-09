#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "http2/http2_local_settings.h"

namespace ruvia::detail {

class http2_ready_queue final {
public:
    [[nodiscard]] bool push(std::uint32_t stream_id) noexcept {
        if (size_ >= stream_ids_.size() && offset_ > 0) {
            // The array is physically full but the consumed prefix is reclaimable.
            // pop() compacts only lazily, so after filling to capacity and popping
            // a few entries the buffer can stay full while holding fewer than
            // capacity live streams. Reclaim the prefix here so a ready stream is
            // never spuriously rejected (which would stall it) while the queue is
            // not logically full.
            const auto remaining = size_ - offset_;
            std::memmove(
                stream_ids_.data(), stream_ids_.data() + offset_, remaining * sizeof(std::uint32_t));
            offset_ = 0;
            size_ = remaining;
        }
        if (size_ >= stream_ids_.size()) {
            return false;
        }
        stream_ids_[size_++] = stream_id;
        return true;
    }

    [[nodiscard]] bool has_ready() const noexcept {
        return offset_ < size_;
    }

    [[nodiscard]] std::uint32_t pop() noexcept {
        const auto stream_id = stream_ids_[offset_++];
        compact();
        return stream_id;
    }

    void remove(std::uint32_t stream_id) noexcept {
        std::size_t write = 0;
        for (std::size_t read = offset_; read < size_; ++read) {
            if (stream_ids_[read] != stream_id) {
                stream_ids_[write++] = stream_ids_[read];
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
        std::memmove(
            stream_ids_.data(), stream_ids_.data() + offset_, remaining * sizeof(std::uint32_t));
        offset_ = 0;
        size_ = remaining;
    }

    std::array<std::uint32_t, http2_local_settings::max_concurrent_streams> stream_ids_{};
    std::size_t size_{0};
    std::size_t offset_{0};
};

}  // namespace ruvia::detail
