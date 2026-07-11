#pragma once

#include <cstdint>

namespace ruvia::detail {

// Shared provenance recorded when a stream leaves active protocol ownership.
// kNone is reserved for lookup misses and inert storage; it is never a live reset.
enum class Http2StreamCloseSource : std::uint8_t {
    kNone,
    kLocal,
    kPeer,
    // The peer's GOAWAY proved this locally initiated request was never processed.
    kPeerGoaway
};

}  // namespace ruvia::detail
