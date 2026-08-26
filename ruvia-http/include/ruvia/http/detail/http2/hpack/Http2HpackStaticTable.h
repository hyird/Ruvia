#pragma once

#include "ruvia/http/HttpStatus.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ruvia::detail {

struct HpackStaticHeader final {
    std::string_view name;
    std::string_view value;
};

struct HpackStaticHeaderMatch final {
    std::uint32_t exactIndex{0};
    std::uint32_t nameIndex{0};
};

inline constexpr auto kHpackStatusOkToken = httpStatusCodeToken(http_status::kOk);
inline constexpr auto kHpackStatusNoContentToken = httpStatusCodeToken(http_status::kNoContent);
inline constexpr auto kHpackStatusPartialContentToken =
    httpStatusCodeToken(http_status::kPartialContent);
inline constexpr auto kHpackStatusNotModifiedToken = httpStatusCodeToken(http_status::kNotModified);
inline constexpr auto kHpackStatusBadRequestToken = httpStatusCodeToken(http_status::kBadRequest);
inline constexpr auto kHpackStatusNotFoundToken = httpStatusCodeToken(http_status::kNotFound);
inline constexpr auto kHpackStatusInternalServerErrorToken =
    httpStatusCodeToken(http_status::kInternalServerError);

// Normative HPACK static table from RFC 7541 Appendix A.
inline constexpr std::array<HpackStaticHeader, 61> kHpackStaticTable{{
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", httpStatusCodeTokenView(kHpackStatusOkToken)},
    {":status", httpStatusCodeTokenView(kHpackStatusNoContentToken)},
    {":status", httpStatusCodeTokenView(kHpackStatusPartialContentToken)},
    {":status", httpStatusCodeTokenView(kHpackStatusNotModifiedToken)},
    {":status", httpStatusCodeTokenView(kHpackStatusBadRequestToken)},
    {":status", httpStatusCodeTokenView(kHpackStatusNotFoundToken)},
    {":status", httpStatusCodeTokenView(kHpackStatusInternalServerErrorToken)},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
}};

inline constexpr std::size_t kHpackStaticTableSize = kHpackStaticTable.size();

[[nodiscard]] inline const HpackStaticHeader& hpackStaticHeaderAt(std::uint32_t index) noexcept {
    return kHpackStaticTable[index - 1];
}

[[nodiscard]] inline HpackStaticHeaderMatch hpackFindStaticHeaderMatch(
    std::string_view name, std::string_view value) noexcept {
    HpackStaticHeaderMatch match;
    for (std::size_t i = 0; i < kHpackStaticTable.size(); ++i) {
        const auto& header = kHpackStaticTable[i];
        if (header.name != name) {
            continue;
        }
        const auto index = static_cast<std::uint32_t>(i + 1);
        if (match.nameIndex == 0) {
            match.nameIndex = index;
        }
        if (header.value == value) {
            match.exactIndex = index;
            return match;
        }
    }
    return match;
}

}  // namespace ruvia::detail
