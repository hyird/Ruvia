#pragma once

#include <cstddef>
#include <coroutine>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Http2OffsetVector.h"
#include "Http2StreamState.h"
#include "ruvia/http/detail/PmrString.h"

namespace ruvia::detail {

inline constexpr std::size_t kHttp2RetainedBodyChunkVectorCapacity = 16;

[[nodiscard]] inline bool http2HasOverflowQueuedStreamBodyChunk(const Http2StreamState& stream) noexcept {
    return stream.bodyChunkOffset < stream.bodyChunks.size();
}

inline void http2EnqueueStreamBodyChunk(Http2StreamState& stream, std::string_view data) {
    if (data.empty()) {
        return;
    }
    if (!stream.hasQueuedBodyChunk && !http2HasOverflowQueuedStreamBodyChunk(stream)) {
        stream.queuedBodyChunk.assign(data.data(), data.size());
        stream.hasQueuedBodyChunk = true;
        return;
    }
    auto& chunk = stream.bodyChunks.emplace_back();
    chunk.assign(data.data(), data.size());
}

inline void http2EnqueueOwnedStreamBodyChunk(Http2StreamState& stream, std::pmr::string& body) {
    if (body.empty()) {
        return;
    }
    if (!stream.hasQueuedBodyChunk && !http2HasOverflowQueuedStreamBodyChunk(stream)) {
        stream.queuedBodyChunk = std::move(body);
        stream.hasQueuedBodyChunk = true;
        body.clear();
        return;
    }
    stream.bodyChunks.push_back(std::move(body));
    body.clear();
}

[[nodiscard]] inline bool http2HasQueuedStreamBodyChunk(const Http2StreamState& stream) noexcept {
    return stream.hasQueuedBodyChunk || http2HasOverflowQueuedStreamBodyChunk(stream);
}

inline void http2CompactBodyChunks(Http2StreamState& stream) {
    http2CompactMovableOffsetVector(stream.bodyChunks, stream.bodyChunkOffset, 16);
    if (!stream.bodyChunks.empty() || stream.bodyChunks.capacity() <= kHttp2RetainedBodyChunkVectorCapacity) {
        return;
    }

    std::pmr::vector<std::pmr::string> empty(stream.bodyChunks.get_allocator());
    stream.bodyChunks.swap(empty);
}

[[nodiscard]] inline bool http2StreamBodyReadReady(const Http2StreamState* stream, bool closing) noexcept {
    return stream == nullptr ||
        stream->reset ||
        closing ||
        http2HasQueuedStreamBodyChunk(*stream) ||
        stream->bodyEnded;
}

[[nodiscard]] inline std::string_view http2PopStreamBodyChunk(Http2StreamState& stream) {
    clearPmrStringRetainingSmall(stream.activeBodyChunk);
    if (stream.hasQueuedBodyChunk) {
        stream.activeBodyChunk.swap(stream.queuedBodyChunk);
        clearPmrStringRetainingSmall(stream.queuedBodyChunk);
        stream.hasQueuedBodyChunk = false;
        return std::string_view(stream.activeBodyChunk);
    }
    if (!http2HasOverflowQueuedStreamBodyChunk(stream)) {
        return {};
    }
    stream.activeBodyChunk = std::move(stream.bodyChunks[stream.bodyChunkOffset++]);
    http2CompactBodyChunks(stream);
    return std::string_view(stream.activeBodyChunk);
}

inline void http2SetBodyWaiter(Http2StreamState& stream, std::coroutine_handle<> continuation) noexcept {
    stream.bodyWaiter = continuation;
}

[[nodiscard]] inline std::coroutine_handle<> http2TakeBodyWaiter(Http2StreamState& stream) noexcept {
    return std::exchange(stream.bodyWaiter, {});
}

}  // namespace ruvia::detail
