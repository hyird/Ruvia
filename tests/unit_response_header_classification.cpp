#include "test_harness.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "HttpResponseKnownHeaders.h"
#include "HttpResponseHeaderBits.h"

namespace {

using ruvia::detail::classifyResponseHeaderName;

struct Case final {
    std::string_view name;
    std::uint32_t bit;
};

const Case kKnown[] = {
    {"Date", ruvia::detail::kResponseHeaderDate},
    {"ETag", ruvia::detail::kResponseHeaderEtag},
    {"Vary", ruvia::detail::kResponseHeaderVary},
    {"Allow", ruvia::detail::kResponseHeaderAllow},
    {"Server", ruvia::detail::kResponseHeaderServer},
    {"Location", ruvia::detail::kResponseHeaderLocation},
    {"Connection", ruvia::detail::kResponseHeaderConnection},
    {"Set-Cookie", ruvia::detail::kResponseHeaderSetCookie},
    {"Content-Type", ruvia::detail::kResponseHeaderContentType},
    {"Accept-Ranges", ruvia::detail::kResponseHeaderAcceptRanges},
    {"Cache-Control", ruvia::detail::kResponseHeaderCacheControl},
    {"Content-Range", ruvia::detail::kResponseHeaderContentRange},
    {"Last-Modified", ruvia::detail::kResponseHeaderLastModified},
    {"Content-Length", ruvia::detail::kResponseHeaderContentLength},
    {"Content-Encoding", ruvia::detail::kResponseHeaderContentEncoding},
    {"Transfer-Encoding", ruvia::detail::kResponseHeaderTransferEncoding},
    {"Access-Control-Max-Age", ruvia::detail::kResponseHeaderAccessControlMaxAge},
    {"Access-Control-Allow-Origin", ruvia::detail::kResponseHeaderAccessControlAllowOrigin},
    {"Access-Control-Allow-Methods", ruvia::detail::kResponseHeaderAccessControlAllowMethods},
    {"Access-Control-Allow-Headers", ruvia::detail::kResponseHeaderAccessControlAllowHeaders},
    {"Access-Control-Expose-Headers", ruvia::detail::kResponseHeaderAccessControlExposeHeaders},
    {"Access-Control-Allow-Credentials", ruvia::detail::kResponseHeaderAccessControlAllowCredentials},
};

}  // namespace

RUVIA_TEST(response_header_classification_table) {
    for (const auto& entry : kKnown) {
        RUVIA_CHECK(classifyResponseHeaderName(entry.name) == entry.bit);
    }
}

RUVIA_TEST(response_header_bits_are_distinct) {
    // Each known response header must map to its own bit; a shared bit would make
    // the known-header dedup conflate two different headers.
    for (std::size_t i = 0; i < std::size(kKnown); ++i) {
        RUVIA_CHECK(kKnown[i].bit != 0U);
        for (std::size_t j = i + 1; j < std::size(kKnown); ++j) {
            RUVIA_CHECK(kKnown[i].bit != kKnown[j].bit);
        }
    }
}

RUVIA_TEST(response_header_classification_case_and_unknown) {
    RUVIA_CHECK(classifyResponseHeaderName("date") == ruvia::detail::kResponseHeaderDate);
    RUVIA_CHECK(classifyResponseHeaderName("SET-COOKIE") == ruvia::detail::kResponseHeaderSetCookie);
    RUVIA_CHECK(classifyResponseHeaderName("content-length") == ruvia::detail::kResponseHeaderContentLength);
    RUVIA_CHECK(classifyResponseHeaderName("") == 0U);
    RUVIA_CHECK(classifyResponseHeaderName("X-Custom") == 0U);
    RUVIA_CHECK(classifyResponseHeaderName("Datex") == 0U);  // 5 bytes, not "Allow"
    RUVIA_CHECK(classifyResponseHeaderName("Vari") == 0U);   // 4 bytes, first 'v', not "Vary"
}
