#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace ruvia::detail {

// One identity for wire classification, cached lookup and descriptor tags.
// Other has no cache slot; the remaining identities map to compact slots.
enum class RequestHeaderKind : std::uint8_t {
    kOther,
    kAccept,
    kAcceptEncoding,
    kAccessControlRequestHeaders,
    kAccessControlRequestMethod,
    kAuthorization,
    kConnection,
    kContentEncoding,
    kContentLength,
    kContentType,
    kCookie,
    kExpect,
    kHost,
    kIfMatch,
    kIfModifiedSince,
    kIfNoneMatch,
    kIfRange,
    kIfUnmodifiedSince,
    kOrigin,
    kRange,
    kSecWebSocketKey,
    kSecWebSocketProtocol,
    kSecWebSocketVersion,
    kTransferEncoding,
    kUpgrade,
    kUserAgent,
    kForwarded,
    kXForwardedFor,
    kXForwardedProto,
    kSecWebSocketExtensions
};

inline constexpr std::size_t kRequestHeaderKindCount =
    std::to_underlying(RequestHeaderKind::kSecWebSocketExtensions) + 1;

[[nodiscard]] inline constexpr std::size_t requestHeaderKindKnownSlot(
    RequestHeaderKind kind) noexcept {
    const auto index = static_cast<std::size_t>(kind);
    return index == 0 ? kRequestHeaderKindCount : index - 1;
}

}  // namespace ruvia::detail
