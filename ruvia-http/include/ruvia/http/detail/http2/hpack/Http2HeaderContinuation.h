#pragma once

#include <cstdint>
#include <utility>

#include "ruvia/http/detail/http2/frame/Http2FrameTypes.h"

namespace ruvia::detail {

enum class Http2HeaderBlockKind : std::uint8_t {
    kInitial,
    kTrailers,
    // The block must still be fully HPACK-decoded for connection-state
    // synchronization, but its HTTP fields do not mutate a live stream.
    kDiscarded
};

// A single header block may span one HEADERS/PUSH_PROMISE plus this many
// CONTINUATION frames (RFC 9113 §6.10 permits an endpoint to limit them). The
// encoded-block byte cap bounds frames that carry payload, but an empty
// CONTINUATION frame adds zero bytes and so slips past it -- an unbounded stream
// of them keeps a block "in progress" forever (the CVE-2024-27316 CONTINUATION
// flood). The sans-I/O core has no clock, so like the rapid-reset and PING budgets
// it needs an explicit frame count. 1024 is far above any real peer: a 256 KiB
// block delivered in 1024 frames averages 256 bytes each, finer fragmentation
// than any client produces, while the flood dies after ~9 KiB of wire garbage.
inline constexpr std::uint32_t kHttp2MaxContinuationFrames = 1024;

class Http2HeaderContinuation final {
public:
    [[nodiscard]] bool active() const noexcept {
        return streamId_ != 0;
    }

    // Count one CONTINUATION frame against the per-block budget. Returns false
    // once the budget is exhausted so the caller can fail the connection.
    [[nodiscard]] bool recordContinuationFrame() noexcept {
        return ++continuationFrames_ <= kHttp2MaxContinuationFrames;
    }

    [[nodiscard]] bool expectsFrameType(std::uint8_t frameType) const noexcept {
        return !active() ||
            frameType == static_cast<std::uint8_t>(Http2FrameType::kContinuation);
    }

    [[nodiscard]] bool matches(std::uint32_t streamId) const noexcept {
        return streamId != 0 && streamId == streamId_;
    }

    void start(std::uint32_t streamId, Http2HeaderBlockKind kind) noexcept {
        streamId_ = streamId;
        kind_ = kind;
        continuationFrames_ = 0;
    }

    void reset() noexcept {
        streamId_ = 0;
        kind_ = Http2HeaderBlockKind::kInitial;
        continuationFrames_ = 0;
    }

    [[nodiscard]] Http2HeaderBlockKind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] Http2HeaderBlockKind finishKind() noexcept {
        const auto kind = kind_;
        reset();
        return kind;
    }

private:
    std::uint32_t streamId_{0};
    std::uint32_t continuationFrames_{0};
    Http2HeaderBlockKind kind_{Http2HeaderBlockKind::kInitial};
};

}  // namespace ruvia::detail
