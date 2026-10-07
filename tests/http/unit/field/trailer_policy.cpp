#include <array>
#include <string>
#include <string_view>

#include "ruvia/http/detail/field/HttpTrailerFields.h"
#include "ruvia/http/detail/server/HttpResponseTrailers.h"

#include "test_harness.h"

RUVIA_TEST(http_trailer_policies_share_framing_and_authentication_restrictions) {
    constexpr std::array names{
        "Host", "Content-Length", "Transfer-Encoding", "Connection",
        "Content-Encoding", "Content-Type", "Cookie", "Expect", "If-Match",
        "If-Modified-Since", "If-None-Match", "If-Range", "If-Unmodified-Since",
        "Range", "Upgrade", "Authorization", "TE", "Trailer", "Keep-Alive",
        "Set-Cookie", "Max-Forwards", "Cache-Control", "Content-Range",
        "Proxy-Connection", "Proxy-Authenticate", "Proxy-Authorization"};
    for (std::string_view name : names) {
        std::string lower(name);
        for (auto& ch : lower) {
            if (ch >= 'A' && ch <= 'Z') {
                ch = static_cast<char>(ch + ('a' - 'A'));
            }
        }
        for (auto spelling : {name, std::string_view(lower)}) {
            RUVIA_CHECK(ruvia::detail::isForbiddenHttpRequestTrailerName(spelling));
            RUVIA_CHECK(ruvia::detail::isForbiddenResponseTrailerName(spelling));
            RUVIA_CHECK(!ruvia::detail::isValidHttpRequestTrailerFieldValue(
                spelling, ruvia::detail::HttpFieldListRole::kSender));
            RUVIA_CHECK(!ruvia::detail::isValidHttpResponseTrailerFieldValue(
                spelling, ruvia::detail::HttpFieldListRole::kSender));
        }
    }
}

RUVIA_TEST(http_trailer_policies_preserve_direction_specific_permissions) {
    for (std::string_view name : {"Accept-Ranges", "Origin", "Access-Control-Request-Headers",
             "Access-Control-Request-Method"}) {
        RUVIA_CHECK(ruvia::detail::isForbiddenHttpRequestTrailerName(name));
        RUVIA_CHECK(!ruvia::detail::isForbiddenResponseTrailerName(name));
    }
    for (std::string_view name : {"Age", "Date", "Vary", "Location", "Retry-After"}) {
        RUVIA_CHECK(!ruvia::detail::isForbiddenHttpRequestTrailerName(name));
        RUVIA_CHECK(ruvia::detail::isForbiddenResponseTrailerName(name));
    }
    for (std::string_view name : {"ETag", "Digest", "X-Checksum", "Proxy-Authorization-Extra"}) {
        RUVIA_CHECK(!ruvia::detail::isForbiddenHttpRequestTrailerName(name));
        RUVIA_CHECK(!ruvia::detail::isForbiddenResponseTrailerName(name));
    }
}
