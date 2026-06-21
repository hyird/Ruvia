#pragma once

#include <cstddef>
#include <coroutine>
#include <memory_resource>
#include <string>
#include <string_view>
#include <utility>

#include "Http2StreamState.h"

namespace ruvia::detail {

inline void http2EnqueueStreamBodyChunk(Http2StreamState& stream, std::string_view data) {
    if (data.empty()) {
        return;
    }
    auto& chunk = stream.bodyChunks.emplace_back();
    chunk.assign(data.data(), data.size());
}

inline void http2EnqueueOwnedStreamBodyChunk(Http2StreamState& stream, std::pmr::string& body) {
    if (body.empty()) {
        return;
    }
    stream.bodyChunks.push_back(std::move(body));
    body.clear();
}

[[nodiscard]] inline bool http2HasQueuedStreamBodyChunk(const Http2StreamState& stream) noexcept {
    return stream.bodyChunkOffset < stream.bodyChunks.size();
}

inline void http2CompactBodyChunks(Http2StreamState& stream) {
    if (stream.bodyChunkOffset == 0) {
        return;
    }
    if (stream.bodyChunkOffset == stream.bodyChunks.size()) {
        stream.bodyChunks.clear();
        stream.bodyChunkOffset = 0;
        return;
    }
    if (stream.bodyChunkOffset < 16 && stream.bodyChunkOffset * 2 < stream.bodyChunks.size()) {
        return;
    }
    const auto remaining = stream.bodyChunks.size() - stream.bodyChunkOffset;
    for (std::size_t i = 0; i < remaining; ++i) {
        stream.bodyChunks[i] = std::move(stream.bodyChunks[stream.bodyChunkOffset + i]);
    }
    stream.bodyChunks.resize(remaining);
    stream.bodyChunkOffset = 0;
}

[[nodiscard]] inline bool http2StreamBodyReadReady(const Http2StreamState* stream, bool closing) noexcept {
    return stream == nullptr ||
        stream->reset ||
        closing ||
        http2HasQueuedStreamBodyChunk(*stream) ||
        stream->bodyEnded;
}

[[nodiscard]] inline std::string_view http2PopStreamBodyChunk(Http2StreamState& stream) {
    stream.activeBodyChunk.clear();
    if (!http2HasQueuedStreamBodyChunk(stream)) {
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
