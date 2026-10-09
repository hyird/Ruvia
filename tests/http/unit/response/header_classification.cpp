#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/detail/response/http_response_header_bits.h"
#include "ruvia/http/detail/response/http_response_known_headers.h"

#include "test_harness.h"

namespace {

using ruvia::detail::classify_response_header_name;

struct case_value final {
    std::string_view name_;
    std::uint32_t bit_;
};

const case_value known[] = {
    {"Date", ruvia::detail::response_header_date},
    {"ETag", ruvia::detail::response_header_etag},
    {"Vary", ruvia::detail::response_header_vary},
    {"Allow", ruvia::detail::response_header_allow},
    {"Server", ruvia::detail::response_header_server},
    {"Location", ruvia::detail::response_header_location},
    {"Connection", ruvia::detail::response_header_connection},
    {"Set-Cookie", ruvia::detail::response_header_set_cookie},
    {"Content-Type", ruvia::detail::response_header_content_type},
    {"Accept-Ranges", ruvia::detail::response_header_accept_ranges},
    {"Cache-Control", ruvia::detail::response_header_cache_control},
    {"Content-Range", ruvia::detail::response_header_content_range},
    {"Last-Modified", ruvia::detail::response_header_last_modified},
    {"Content-Length", ruvia::detail::response_header_content_length},
    {"Content-Encoding", ruvia::detail::response_header_content_encoding},
    {"Transfer-Encoding", ruvia::detail::response_header_transfer_encoding},
    {"Access-Control-Max-Age", ruvia::detail::response_header_access_control_max_age},
    {"Access-Control-Allow-Origin", ruvia::detail::response_header_access_control_allow_origin},
    {"Access-Control-Allow-Methods", ruvia::detail::response_header_access_control_allow_methods},
    {"Access-Control-Allow-Headers", ruvia::detail::response_header_access_control_allow_headers},
    {"Access-Control-Expose-Headers", ruvia::detail::response_header_access_control_expose_headers},
    {"Access-Control-Allow-Credentials",
        ruvia::detail::response_header_access_control_allow_credentials},
};

}  // namespace

RUVIA_TEST(response_header_classification_table) {
    for (const auto& entry : known) {
        RUVIA_CHECK(classify_response_header_name(entry.name_) == entry.bit_);
    }
}

RUVIA_TEST(response_header_bits_are_distinct) {
    // Each known response header must map to its own bit; a shared bit would make
    // the known-header dedup conflate two different headers.
    for (std::size_t i = 0; i < std::size(known); ++i) {
        RUVIA_CHECK(known[i].bit_ != 0U);
        for (std::size_t j = i + 1; j < std::size(known); ++j) {
            RUVIA_CHECK(known[i].bit_ != known[j].bit_);
        }
    }
}

RUVIA_TEST(response_header_classification_case_and_unknown) {
    RUVIA_CHECK(classify_response_header_name("date") == ruvia::detail::response_header_date);
    RUVIA_CHECK(
        classify_response_header_name("SET-COOKIE") == ruvia::detail::response_header_set_cookie);
    RUVIA_CHECK(classify_response_header_name("content-length") ==
                ruvia::detail::response_header_content_length);
    RUVIA_CHECK(classify_response_header_name("") == 0U);
    RUVIA_CHECK(classify_response_header_name("X-Custom") == 0U);
    RUVIA_CHECK(classify_response_header_name("Datex") == 0U);  // 5 bytes, not "Allow"
    RUVIA_CHECK(classify_response_header_name("Vari") == 0U);   // 4 bytes, first 'v', not "Vary"
}
