#include <array>
#include <cstdint>
#include <ctime>
#include <limits>
#include <string>
#include <utility>

#include "ruvia/http/http_cache.h"
#include "ruvia/http/http_date.h"

#include "test_harness.h"

namespace {

std::string format_date(std::time_t time) {
    const auto out = ruvia::format_http_date(time).value();
    return std::string(out.data(), out.size());
}

}  // namespace

RUVIA_TEST(http_format_date_known_vectors) {
    RUVIA_CHECK_EQ(format_date(0), std::string("Thu, 01 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(format_date(86400), std::string("Fri, 02 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(format_date(784111777), std::string("Sun, 06 Nov 1994 08:49:37 GMT"));
    RUVIA_CHECK_EQ(format_date(172800), std::string("Sat, 03 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(format_date(345600), std::string("Mon, 05 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(format_date(432000), std::string("Tue, 06 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(format_date(518400), std::string("Wed, 07 Jan 1970 00:00:00 GMT"));
}

RUVIA_TEST(http_format_date_round_trips_through_public_parser) {
    for (const auto time : std::array<std::int64_t, 5>{
             -315619200, -1, 0, 784111777, 253402300799}) {
        if (!std::in_range<std::time_t>(time)) {
            continue;
        }
        const auto parsed = ruvia::parse_http_date(format_date(static_cast<std::time_t>(time)));
        RUVIA_CHECK(parsed.has_value());
        RUVIA_CHECK_EQ(static_cast<std::int64_t>(*parsed), time);
    }
}

RUVIA_TEST(http_date_conversion_rejects_unrepresentable_years) {
    for (const auto time : {std::int64_t{-62167219201LL}, std::int64_t{253402300800LL},
             std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
        if (!std::in_range<std::time_t>(time)) {
            continue;
        }
        RUVIA_CHECK(!ruvia::format_http_date(static_cast<std::time_t>(time)));
    }
    RUVIA_CHECK_EQ(format_date(-315619200), std::string("Fri, 01 Jan 1960 00:00:00 GMT"));
    RUVIA_CHECK_EQ(format_date(-1), std::string("Wed, 31 Dec 1969 23:59:59 GMT"));
    if (std::in_range<std::time_t>(253402300799LL)) {
        RUVIA_CHECK_EQ(format_date(static_cast<std::time_t>(253402300799LL)), std::string("Fri, 31 Dec 9999 23:59:59 GMT"));
    }
}
