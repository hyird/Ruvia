#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <utility>
#include <vector>

#include "Http2Frame.h"
#include "Http2StreamState.h"

namespace ruvia::detail {

[[nodiscard]] inline Http2StreamState* http2FindStream(
    std::pmr::vector<Http2StreamState>& streams,
    std::uint32_t streamId) noexcept {
    for (auto& stream : streams) {
        if (stream.id == streamId) {
            return &stream;
        }
    }
    return nullptr;
}

[[nodiscard]] inline const Http2StreamState* http2FindStream(
    const std::pmr::vector<Http2StreamState>& streams,
    std::uint32_t streamId) noexcept {
    for (const auto& stream : streams) {
        if (stream.id == streamId) {
            return &stream;
        }
    }
    return nullptr;
}

[[nodiscard]] inline bool http2IsIdleStream(std::uint32_t streamId, std::uint32_t lastStreamId) noexcept {
    return streamId > lastStreamId || (streamId & 1U) == 0;
}

[[nodiscard]] inline Http2StreamState* http2CreateStream(
    std::pmr::vector<Http2StreamState>& streams,
    std::uint32_t streamId,
    std::pmr::memory_resource* resource,
    std::int32_t peerInitialWindowSize) {
    if (auto* existing = http2FindStream(streams, streamId); existing != nullptr) {
        return existing;
    }
    if (streams.size() >= kHttp2LocalMaxConcurrentStreams) {
        return nullptr;
    }
    streams.emplace_back(streamId, resource);
    streams.back().sendWindow = peerInitialWindowSize;
    return &streams.back();
}

inline void http2EraseStreamAt(std::pmr::vector<Http2StreamState>& streams, std::size_t index) noexcept {
    if (index + 1 != streams.size()) {
        streams[index] = std::move(streams.back());
    }
    streams.pop_back();
}

inline bool http2RemoveStream(std::pmr::vector<Http2StreamState>& streams, std::uint32_t streamId) noexcept {
    for (std::size_t i = 0; i < streams.size(); ++i) {
        if (streams[i].id == streamId) {
            http2EraseStreamAt(streams, i);
            return true;
        }
    }
    return false;
}

inline bool http2ApplyStreamSendWindowDelta(
    std::pmr::vector<Http2StreamState>& streams,
    std::int64_t delta) noexcept {
    for (auto& stream : streams) {
        const auto updated = static_cast<std::int64_t>(stream.sendWindow) + delta;
        if (updated > std::numeric_limits<std::int32_t>::max() ||
            updated < std::numeric_limits<std::int32_t>::min()) {
            return false;
        }
        stream.sendWindow = static_cast<std::int32_t>(updated);
    }
    return true;
}

}  // namespace ruvia::detail
