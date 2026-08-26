#pragma once

#include <cstdint>

namespace ruvia::detail {

// Shared provenance recorded when a stream leaves active protocol ownership.
// Absence belongs to the owning discriminated state or optional lookup result,
// so every enumerator is a real close source.
enum class Http2StreamCloseSource : std::uint8_t {
    kLocal,
    kPeer,
    // The peer's GOAWAY proved this locally initiated request was never processed.
    kPeerGoaway
};

[[nodiscard]] constexpr bool http2IsValidStreamCloseSource(Http2StreamCloseSource source) noexcept {
    return source == Http2StreamCloseSource::kLocal || source == Http2StreamCloseSource::kPeer ||
           source == Http2StreamCloseSource::kPeerGoaway;
}

}  // namespace ruvia::detail
