#pragma once

#include "ruvia/http/detail/http2/stream/Http2StreamState.h"

namespace ruvia::detail {

// Cross-phase queries over the exclusive remote-receive alternatives. Keeping
// these in one inlinable protocol contract prevents receive and submission paths
// from reconstructing subtly different head/tunnel half-close predicates.
[[nodiscard]] inline bool http2RemoteFinalHeadDecoded(const Http2StreamState& stream) noexcept {
    const auto& remote = stream.remoteReceive();
    return remote.headPending() == nullptr && remote.headEndStreamPending() == nullptr;
}

[[nodiscard]] inline bool http2RemotePeerHalfClosed(const Http2StreamState& stream) noexcept {
    const auto& remote = stream.remoteReceive();
    return remote.headEndStreamPending() != nullptr ||
           remote.connectPendingEndStream() != nullptr || remote.endStream() != nullptr ||
           remote.aborted() != nullptr;
}

// Protocol closure is independent of whether request-view storage remains pinned.
// A reset closes both halves immediately; otherwise both peer END_STREAM and a
// committed local END_STREAM are required.
[[nodiscard]] inline bool http2StreamIsClosed(const Http2StreamState& stream) noexcept {
    return stream.isAborted() || (http2RemotePeerHalfClosed(stream) &&
                                     stream.localSend().endStreamCommitted() != nullptr);
}

}  // namespace ruvia::detail
