#pragma once

#include <coroutine>
#include <cstddef>
#include <memory_resource>
#include <vector>

#include "Http2OffsetVector.h"

namespace ruvia::detail {

class Http2ContinuationQueue final {
public:
    explicit Http2ContinuationQueue(std::pmr::memory_resource* resource)
        : continuations_(resource == nullptr ? std::pmr::get_default_resource() : resource) {
        continuations_.reserve(16);
    }

    void push(std::coroutine_handle<> continuation) {
        continuations_.push_back(continuation);
    }

    void resumeNext() {
        if (offset_ >= continuations_.size()) {
            continuations_.clear();
            offset_ = 0;
            return;
        }
        auto continuation = continuations_[offset_++];
        http2CompactOffsetVector(continuations_, offset_, 64);
        if (continuation) {
            continuation.resume();
        }
    }

    void resumeAll() {
        while (offset_ < continuations_.size()) {
            auto continuation = continuations_[offset_++];
            if (continuation) {
                continuation.resume();
            }
        }
        continuations_.clear();
        offset_ = 0;
    }

    void resumeAllCurrent() {
        const auto resumeCount = continuations_.size();
        std::size_t offset = 0;
        while (offset < resumeCount) {
            auto continuation = continuations_[offset++];
            if (continuation) {
                continuation.resume();
            }
        }
        http2CompactOffsetVector(continuations_, offset, 0);
    }

private:
    std::pmr::vector<std::coroutine_handle<>> continuations_;
    std::size_t offset_{0};
};

}  // namespace ruvia::detail
