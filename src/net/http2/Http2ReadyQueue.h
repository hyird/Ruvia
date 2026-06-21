#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <vector>

#include "Http2OffsetVector.h"

namespace ruvia::detail {

class Http2ReadyQueue final {
public:
    explicit Http2ReadyQueue(std::pmr::memory_resource* resource)
        : streamIds_(resource == nullptr ? std::pmr::get_default_resource() : resource) {
        streamIds_.reserve(16);
    }

    void push(std::uint32_t streamId) {
        streamIds_.push_back(streamId);
    }

    [[nodiscard]] bool hasReady() const noexcept {
        return offset_ < streamIds_.size();
    }

    [[nodiscard]] std::uint32_t pop() noexcept {
        const auto streamId = streamIds_[offset_++];
        compact();
        return streamId;
    }

    void remove(std::uint32_t streamId) noexcept {
        const auto activeBegin = streamIds_.begin() + static_cast<std::ptrdiff_t>(offset_);
        streamIds_.erase(
            std::remove(activeBegin, streamIds_.end(), streamId),
            streamIds_.end());
        compact();
    }

private:
    void compact() noexcept {
        http2CompactOffsetVector(streamIds_, offset_, 64);
    }

    std::pmr::vector<std::uint32_t> streamIds_;
    std::size_t offset_{0};
};

}  // namespace ruvia::detail
