#pragma once

#include <cstddef>
#include <coroutine>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/detail/http2/Http2OffsetVector.h"
#include "ruvia/http/detail/http2/Http2StreamState.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {

inline constexpr std::size_t kHttp2RetainedBodyChunkVectorCapacity = 16;

inline bool Http2StreamBodyQueue::hasOverflowQueuedChunk() const noexcept {
    return overflowChunkOffset_ < overflowChunks_.size();
}

inline void Http2StreamBodyQueue::enqueue(std::string_view data) {
    if (data.empty()) {
        return;
    }
    queuedBytes_ += data.size();
    if (!hasQueuedChunk_ && !hasOverflowQueuedChunk()) {
        queuedChunk_.assign(data.data(), data.size());
        hasQueuedChunk_ = true;
        return;
    }
    auto& chunk = overflowChunks_.emplace_back();
    chunk.assign(data.data(), data.size());
}

inline void Http2StreamBodyQueue::enqueueOwned(std::pmr::string& body) {
    if (body.empty()) {
        return;
    }
    queuedBytes_ += body.size();
    if (!hasQueuedChunk_ && !hasOverflowQueuedChunk()) {
        queuedChunk_ = std::move(body);
        hasQueuedChunk_ = true;
        body.clear();
        return;
    }
    overflowChunks_.push_back(std::move(body));
    body.clear();
}

inline void Http2StreamRequestData::moveBodyToQueue(Http2StreamBodyQueue& queue) {
    queue.enqueueOwned(body_);
}

inline bool Http2StreamBodyQueue::hasQueuedChunk() const noexcept {
    return hasQueuedChunk_ || hasOverflowQueuedChunk();
}

inline void Http2StreamBodyQueue::compact() {
    http2CompactMovableOffsetVector(overflowChunks_, overflowChunkOffset_, 16);
    if (!overflowChunks_.empty() || overflowChunks_.capacity() <= kHttp2RetainedBodyChunkVectorCapacity) {
        return;
    }

    std::pmr::vector<std::pmr::string> empty(overflowChunks_.get_allocator());
    overflowChunks_.swap(empty);
}

inline std::string_view Http2StreamBodyQueue::pop() {
    clearPmrStringRetainingSmall(activeChunk_);
    if (hasQueuedChunk_) {
        activeChunk_.swap(queuedChunk_);
        queuedBytes_ -= activeChunk_.size();
        clearPmrStringRetainingSmall(queuedChunk_);
        hasQueuedChunk_ = false;
        return std::string_view(activeChunk_);
    }
    if (!hasOverflowQueuedChunk()) {
        return {};
    }
    activeChunk_ = std::move(overflowChunks_[overflowChunkOffset_++]);
    queuedBytes_ -= activeChunk_.size();
    compact();
    return std::string_view(activeChunk_);
}

inline std::size_t Http2StreamBodyQueue::queuedBytes() const noexcept {
    return queuedBytes_;
}

inline void Http2StreamBodyQueue::setWaiter(std::coroutine_handle<> continuation) noexcept {
    waiter_ = continuation;
}

inline std::coroutine_handle<> Http2StreamBodyQueue::takeWaiter() noexcept {
    return std::exchange(waiter_, {});
}

[[nodiscard]] inline bool http2HasOverflowQueuedStreamBodyChunk(const Http2StreamState& stream) noexcept {
    return stream.hasOverflowQueuedBodyChunk();
}

inline void http2EnqueueStreamBodyChunk(Http2StreamState& stream, std::string_view data) {
    stream.enqueueBodyChunk(data);
}

inline void http2EnqueueOwnedStreamBodyChunk(Http2StreamState& stream, std::pmr::string& body) {
    stream.enqueueOwnedBodyChunk(body);
}

inline void http2EnqueueBufferedRequestBodyChunk(Http2StreamState& stream) {
    stream.enqueueBufferedRequestBodyChunk();
}

[[nodiscard]] inline bool http2HasQueuedStreamBodyChunk(const Http2StreamState& stream) noexcept {
    return stream.hasQueuedBodyChunk();
}

inline void http2CompactBodyChunks(Http2StreamState& stream) {
    stream.compactBodyChunks();
}

[[nodiscard]] inline bool http2StreamBodyReadReady(const Http2StreamState* stream, bool closing) noexcept {
    return stream == nullptr ||
        stream->isAborted() ||
        closing ||
        http2HasQueuedStreamBodyChunk(*stream) ||
        stream->bodyEnded();
}

[[nodiscard]] inline std::string_view http2PopStreamBodyChunk(Http2StreamState& stream) {
    return stream.popBodyChunk();
}

inline void http2SetBodyWaiter(Http2StreamState& stream, std::coroutine_handle<> continuation) noexcept {
    stream.setBodyWaiter(continuation);
}

[[nodiscard]] inline std::coroutine_handle<> http2TakeBodyWaiter(Http2StreamState& stream) noexcept {
    return stream.takeBodyWaiter();
}

}  // namespace ruvia::detail
