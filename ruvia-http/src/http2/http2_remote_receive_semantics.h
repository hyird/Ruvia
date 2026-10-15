#pragma once

#include "http2/http2_stream_state.h"

namespace ruvia::detail {

// Cross-phase queries over the exclusive remote-receive alternatives. Keeping
// these in one inlinable protocol contract prevents receive and submission paths
// from reconstructing subtly different head/tunnel half-close predicates.
[[nodiscard]] inline bool http2_remote_final_head_decoded(const http2_stream_state& stream) noexcept {
    const auto& remote = stream.remote_receive();
    return remote.head_pending() == nullptr && remote.head_end_stream_pending() == nullptr;
}

[[nodiscard]] inline bool http2_remote_peer_half_closed(const http2_stream_state& stream) noexcept {
    const auto& remote = stream.remote_receive();
    return remote.head_end_stream_pending() != nullptr ||
           remote.connect_pending_end_stream() != nullptr || remote.end_stream() != nullptr ||
           remote.aborted() != nullptr;
}

// Protocol closure is independent of whether request-view storage remains pinned.
// A reset closes both halves immediately; otherwise both peer END_STREAM and a
// committed local END_STREAM are required.
[[nodiscard]] inline bool http2_stream_is_closed(const http2_stream_state& stream) noexcept {
    return stream.is_aborted() || (http2_remote_peer_half_closed(stream) &&
                                      stream.local_send().end_stream_committed() != nullptr);
}

}  // namespace ruvia::detail
