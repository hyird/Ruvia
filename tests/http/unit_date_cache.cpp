#include "test_harness.h"

#include <cstddef>
#include <ctime>
#include <optional>
#include <string_view>

#include "ruvia/http/detail/server/HttpDateCache.h"
#include "ruvia/http/detail/HttpDate.h"

namespace {

using ruvia::detail::cachedDateHeader;
using ruvia::detail::cachedDateValue;
using ruvia::detail::httpParseImfFixdate;

}  // namespace

RUVIA_TEST(cached_date_header_is_well_formed) {
    const auto header = cachedDateHeader();
    // "Date: " (6) + IMF-fixdate (29) + CRLF (2) = 37 bytes.
    RUVIA_CHECK_EQ(header.size(), std::size_t{37});
    RUVIA_CHECK(header.starts_with("Date: "));
    RUVIA_CHECK(header.ends_with("\r\n"));
    RUVIA_CHECK_EQ(cachedDateValue().size(), std::size_t{29});
}

RUVIA_TEST(cached_date_value_parses_to_current_time) {
    // Bracket the read: the cached second must fall within [before, after].
    const auto before = std::time(nullptr);
    const auto value = cachedDateValue();
    const auto after = std::time(nullptr);

    const auto parsed = httpParseImfFixdate(value);
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK(*parsed >= before - 2);
    RUVIA_CHECK(*parsed <= after + 2);
}
