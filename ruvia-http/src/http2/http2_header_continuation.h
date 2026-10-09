#pragma once

#include <cstdint>
#include <utility>

#include "http2/http2_frame_types.h"

namespace ruvia::detail {

enum class http2_header_block_kind : std::uint8_t {
    initial,
    trailers,
    push_promise,
    // The block must still be fully HPACK-decoded for connection-state
    // synchronization, but its HTTP fields do not mutate a live stream.
    discarded
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
inline constexpr std::uint32_t http2_max_continuation_frames = 1024;

class http2_header_continuation final {
public:
    struct checkpoint_type final {
        std::uint32_t stream_id_;
        std::uint32_t continuation_frames_;
        http2_header_block_kind kind_;
    };

    [[nodiscard]] bool active() const noexcept {
        return stream_id_ != 0;
    }

    // Check the budget without changing it. The caller performs any fallible
    // fragment buffering first, then commits the count so a recoverable PMR
    // failure leaves the exact CONTINUATION frame retryable.
    [[nodiscard]] bool continuation_frame_budget_available() const noexcept {
        return continuation_frames_ < http2_max_continuation_frames;
    }

    // Count one already-buffered CONTINUATION frame against the per-block budget.
    [[nodiscard]] bool record_continuation_frame() noexcept {
        return ++continuation_frames_ <= http2_max_continuation_frames;
    }

    [[nodiscard]] bool expects_frame_type(std::uint8_t frame_type) const noexcept {
        return !active() || frame_type == static_cast<std::uint8_t>(http2_frame_type::continuation);
    }

    [[nodiscard]] bool matches(std::uint32_t stream_id) const noexcept {
        return stream_id != 0 && stream_id == stream_id_;
    }

    void start(std::uint32_t stream_id, http2_header_block_kind kind) noexcept {
        stream_id_ = stream_id;
        kind_ = kind;
        continuation_frames_ = 0;
    }

    void reset() noexcept {
        stream_id_ = 0;
        kind_ = http2_header_block_kind::initial;
        continuation_frames_ = 0;
    }

    [[nodiscard]] http2_header_block_kind kind() const noexcept {
        return kind_;
    }

    [[nodiscard]] checkpoint_type checkpoint() const noexcept {
        return checkpoint_type{stream_id_, continuation_frames_, kind_};
    }

    void restore(checkpoint_type checkpoint) noexcept {
        stream_id_ = checkpoint.stream_id_;
        continuation_frames_ = checkpoint.continuation_frames_;
        kind_ = checkpoint.kind_;
    }

    [[nodiscard]] http2_header_block_kind finish_kind() noexcept {
        const auto kind = kind_;
        reset();
        return kind;
    }

private:
    std::uint32_t stream_id_{0};
    std::uint32_t continuation_frames_{0};
    http2_header_block_kind kind_{http2_header_block_kind::initial};
};

}  // namespace ruvia::detail
