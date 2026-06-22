#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace ruvia::detail {

inline constexpr std::uint32_t kResponseHeaderContentLength = 1U << 0;
inline constexpr std::uint32_t kResponseHeaderContentEncoding = 1U << 1;
inline constexpr std::uint32_t kResponseHeaderContentType = 1U << 2;
inline constexpr std::uint32_t kResponseHeaderConnection = 1U << 3;
inline constexpr std::uint32_t kResponseHeaderVary = 1U << 4;
inline constexpr std::uint32_t kResponseHeaderDate = 1U << 5;
inline constexpr std::uint32_t kResponseHeaderServer = 1U << 6;
inline constexpr std::uint32_t kResponseHeaderCacheControl = 1U << 7;
inline constexpr std::uint32_t kResponseHeaderTransferEncoding = 1U << 8;
inline constexpr std::uint32_t kResponseHeaderAllow = 1U << 9;
inline constexpr std::uint32_t kResponseHeaderAccessControlAllowOrigin = 1U << 10;
inline constexpr std::uint32_t kResponseHeaderAccessControlAllowCredentials = 1U << 11;
inline constexpr std::uint32_t kResponseHeaderAccessControlAllowMethods = 1U << 12;
inline constexpr std::uint32_t kResponseHeaderAccessControlAllowHeaders = 1U << 13;
inline constexpr std::uint32_t kResponseHeaderAccessControlMaxAge = 1U << 14;
inline constexpr std::uint32_t kResponseHeaderAccessControlExposeHeaders = 1U << 15;
inline constexpr std::uint32_t kResponseHeaderAcceptRanges = 1U << 16;
inline constexpr std::uint32_t kResponseHeaderContentRange = 1U << 17;
inline constexpr std::uint32_t kResponseHeaderEtag = 1U << 18;
inline constexpr std::uint32_t kResponseHeaderLastModified = 1U << 19;
inline constexpr std::uint32_t kResponseHeaderLocation = 1U << 20;
inline constexpr std::uint32_t kResponseHeaderSetCookie = 1U << 21;
inline constexpr std::size_t kResponseKnownHeaderCount = 22;

[[nodiscard]] inline std::size_t responseKnownHeaderSlot(std::uint32_t bit) noexcept {
    constexpr std::uint32_t knownMask = (1U << kResponseKnownHeaderCount) - 1U;
    if (bit == 0 || (bit & ~knownMask) != 0 || (bit & (bit - 1U)) != 0) {
        return kResponseKnownHeaderCount;
    }
    return static_cast<std::size_t>(std::countr_zero(bit));
}

}  // namespace ruvia::detail
