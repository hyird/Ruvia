#pragma once

#include <array>
#include <coroutine>
#include <cstddef>
#include <memory_resource>
#include <vector>

#include "Http2OffsetVector.h"
#include "ruvia/memory/PmrResource.h"

namespace ruvia::detail {

class Http2ContinuationQueue final {
public:
    explicit Http2ContinuationQueue(std::pmr::memory_resource* resource)
        : overflow_(pmrResourceOrDefault(resource)) {}

    void push(std::coroutine_handle<> continuation) {
        if (overflowHasActive() || !tryPushInline(continuation)) {
            overflow_.push_back(continuation);
        }
    }

    void resumeNext() {
        auto continuation = popNext();
        if (!continuation) {
            return;
        }
        continuation.resume();
    }

    void resumeAll() {
        while (hasActive()) {
            resumeNext();
        }
        clear();
    }

    void resumeAllCurrent() {
        SnapshotResumeGuard guard(*this);
        const auto inlineEnd = inlineSize_;
        const auto overflowEnd = overflow_.size();
        while (inlineOffset_ < inlineEnd) {
            auto continuation = inline_[inlineOffset_++];
            if (continuation) {
                continuation.resume();
            }
        }
        compactInline();
        while (overflowOffset_ < overflowEnd) {
            auto continuation = overflow_[overflowOffset_++];
            if (continuation) {
                continuation.resume();
            }
        }
        compactOverflow(0);
    }

private:
    static constexpr std::size_t kInlineCapacity = 16;

    class SnapshotResumeGuard final {
    public:
        explicit SnapshotResumeGuard(Http2ContinuationQueue& queue) noexcept
            : queue_(queue) {
            ++queue_.snapshotResumeDepth_;
        }

        SnapshotResumeGuard(const SnapshotResumeGuard&) = delete;
        SnapshotResumeGuard& operator=(const SnapshotResumeGuard&) = delete;

        ~SnapshotResumeGuard() {
            --queue_.snapshotResumeDepth_;
        }

    private:
        Http2ContinuationQueue& queue_;
    };

    [[nodiscard]] bool inlineHasActive() const noexcept {
        return inlineOffset_ < inlineSize_;
    }

    [[nodiscard]] bool overflowHasActive() const noexcept {
        return overflowOffset_ < overflow_.size();
    }

    [[nodiscard]] bool hasActive() const noexcept {
        return inlineHasActive() || overflowHasActive();
    }

    [[nodiscard]] bool isResumingSnapshot() const noexcept {
        return snapshotResumeDepth_ != 0;
    }

    bool tryPushInline(std::coroutine_handle<> continuation) noexcept {
        if (!isResumingSnapshot()) {
            compactInline();
        }
        if (inlineSize_ >= inline_.size()) {
            return false;
        }
        inline_[inlineSize_++] = continuation;
        return true;
    }

    [[nodiscard]] std::coroutine_handle<> popNext() {
        if (inlineHasActive()) {
            auto continuation = inline_[inlineOffset_++];
            compactInline();
            return continuation;
        }
        if (overflowHasActive()) {
            auto continuation = overflow_[overflowOffset_++];
            compactOverflow(64);
            return continuation;
        }
        clear();
        return {};
    }

    void compactInline() noexcept {
        if (inlineOffset_ == 0) {
            return;
        }
        if (inlineOffset_ == inlineSize_) {
            inlineOffset_ = 0;
            inlineSize_ = 0;
            return;
        }
        const auto remaining = inlineSize_ - inlineOffset_;
        for (std::size_t i = 0; i < remaining; ++i) {
            inline_[i] = inline_[inlineOffset_ + i];
        }
        inlineOffset_ = 0;
        inlineSize_ = remaining;
    }

    void compactOverflow(std::size_t threshold) {
        http2CompactOffsetVector(overflow_, overflowOffset_, threshold);
    }

    void clear() noexcept {
        inlineOffset_ = 0;
        inlineSize_ = 0;
        overflow_.clear();
        overflowOffset_ = 0;
    }

    std::array<std::coroutine_handle<>, kInlineCapacity> inline_{};
    std::size_t inlineSize_{0};
    std::size_t inlineOffset_{0};
    std::pmr::vector<std::coroutine_handle<>> overflow_;
    std::size_t overflowOffset_{0};
    std::size_t snapshotResumeDepth_{0};
};

}  // namespace ruvia::detail
