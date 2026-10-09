#include <cstddef>
#include <ctime>
#include <optional>
#include <string_view>

#include "field/http_date.h"
#include "server/http_date_cache.h"
#include "test_harness.h"

namespace {

using ruvia::detail::cached_date_header;
using ruvia::detail::cached_date_value;
using ruvia::detail::http_parse_imf_fixdate;

}  // namespace

RUVIA_TEST(cached_date_header_is_well_formed) {
    const auto header_value = cached_date_header();
    // "Date: " (6) + IMF-fixdate (29) + CRLF (2) = 37 bytes.
    RUVIA_CHECK_EQ(header_value.size(), std::size_t{37});
    RUVIA_CHECK(header_value.starts_with("Date: "));
    RUVIA_CHECK(header_value.ends_with("\r\n"));
    RUVIA_CHECK_EQ(cached_date_value().size(), std::size_t{29});
}

RUVIA_TEST(cached_date_value_parses_to_current_time) {
    // Bracket the read: the cached second must fall within [before, after].
    const auto before = std::time(nullptr);
    const auto value = cached_date_value();
    const auto after = std::time(nullptr);

    const auto parsed_value = http_parse_imf_fixdate(value);
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK(*parsed_value >= before - 2);
    RUVIA_CHECK(*parsed_value <= after + 2);
}
