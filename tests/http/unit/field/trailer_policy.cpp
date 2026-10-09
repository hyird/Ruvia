#include <array>
#include <string>
#include <string_view>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/server/http_response_trailers.h"

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
            RUVIA_CHECK(ruvia::detail::is_forbidden_http_request_trailer_name(spelling));
            RUVIA_CHECK(ruvia::detail::is_forbidden_response_trailer_name(spelling));
            RUVIA_CHECK(!ruvia::detail::is_valid_http_request_trailer_field_value(
                spelling, ruvia::detail::http_field_list_role::sender));
            RUVIA_CHECK(!ruvia::detail::is_valid_http_response_trailer_field_value(
                spelling, ruvia::detail::http_field_list_role::sender));
        }
    }
}

RUVIA_TEST(http_trailer_policies_preserve_direction_specific_permissions) {
    for (std::string_view name : {"Accept-Ranges", "Origin", "Access-Control-Request-Headers",
             "Access-Control-Request-Method"}) {
        RUVIA_CHECK(ruvia::detail::is_forbidden_http_request_trailer_name(name));
        RUVIA_CHECK(!ruvia::detail::is_forbidden_response_trailer_name(name));
    }
    for (std::string_view name : {"Age", "Date", "Vary", "Location", "Retry-After"}) {
        RUVIA_CHECK(!ruvia::detail::is_forbidden_http_request_trailer_name(name));
        RUVIA_CHECK(ruvia::detail::is_forbidden_response_trailer_name(name));
    }
    for (std::string_view name : {"ETag", "Digest", "X-Checksum", "Proxy-Authorization-Extra"}) {
        RUVIA_CHECK(!ruvia::detail::is_forbidden_http_request_trailer_name(name));
        RUVIA_CHECK(!ruvia::detail::is_forbidden_response_trailer_name(name));
    }
}
