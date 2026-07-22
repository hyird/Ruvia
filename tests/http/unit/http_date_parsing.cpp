#include "test_harness.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/detail/field/HttpDate.h"
#include "ruvia/http/detail/field/HttpEntityTag.h"
#include "ruvia/web/detail/http/StaticFileMetadata.h"

namespace {

using ruvia::detail::guessStaticFileContentType;

std::string_view guess(const char* name) {
    return guessStaticFileContentType(std::filesystem::path(name));
}

}  // namespace

// Reading an HTTP date: IMF-fixdate and the two obsolete formats a recipient must still accept.

RUVIA_TEST(http_month_index_lookup) {
    using ruvia::detail::httpMonthIndex;
    RUVIA_CHECK_EQ(httpMonthIndex("Jan"), 1);
    RUVIA_CHECK_EQ(httpMonthIndex("Feb"), 2);
    RUVIA_CHECK_EQ(httpMonthIndex("Nov"), 11);
    RUVIA_CHECK_EQ(httpMonthIndex("Dec"), 12);
    // Unknown, empty, and wrong-case names are rejected (0).
    RUVIA_CHECK_EQ(httpMonthIndex("Xyz"), 0);
    RUVIA_CHECK_EQ(httpMonthIndex(""), 0);
    RUVIA_CHECK_EQ(httpMonthIndex("jan"), 0);
}

RUVIA_TEST(http_parse_fixed_digits) {
    using ruvia::detail::httpParseFixedDigits;
    RUVIA_CHECK_EQ(httpParseFixedDigits("07").value_or(-1), 7);
    RUVIA_CHECK_EQ(httpParseFixedDigits("2024").value_or(-1), 2024);
    RUVIA_CHECK_EQ(httpParseFixedDigits("00").value_or(-1), 0);
    // Empty and any non-digit byte are rejected.
    RUVIA_CHECK(!httpParseFixedDigits("").has_value());
    RUVIA_CHECK(!httpParseFixedDigits("1a").has_value());
    RUVIA_CHECK(!httpParseFixedDigits(" 7").has_value());
}

RUVIA_TEST(http_days_from_civil_epoch) {
    using ruvia::detail::httpDaysFromCivil;
    // Days relative to the Unix epoch (Howard Hinnant's algorithm).
    RUVIA_CHECK_EQ(httpDaysFromCivil(1970, 1, 1), std::int64_t{0});
    RUVIA_CHECK_EQ(httpDaysFromCivil(1970, 1, 2), std::int64_t{1});
    RUVIA_CHECK_EQ(httpDaysFromCivil(1969, 12, 31), std::int64_t{-1});
    RUVIA_CHECK_EQ(httpDaysFromCivil(2000, 1, 1), std::int64_t{10957});
    RUVIA_CHECK_EQ(httpDaysFromCivil(1994, 11, 6), std::int64_t{9075});
}

RUVIA_TEST(http_parse_imf_fixdate) {
    using ruvia::detail::httpParseImfFixdate;
    // The canonical RFC 7231 example resolves to its known epoch second.
    const auto canonical = httpParseImfFixdate("Sun, 06 Nov 1994 08:49:37 GMT");
    RUVIA_CHECK(canonical.has_value());
    RUVIA_CHECK_EQ(*canonical, std::time_t{784111777});
    // The Unix epoch itself.
    const auto epoch = httpParseImfFixdate("Thu, 01 Jan 1970 00:00:00 GMT");
    RUVIA_CHECK(epoch.has_value());
    RUVIA_CHECK_EQ(*epoch, std::time_t{0});
    // A leap second (60) is accepted.
    RUVIA_CHECK(httpParseImfFixdate("Sun, 06 Nov 1994 08:49:60 GMT").has_value());

    // Malformed inputs are rejected.
    RUVIA_CHECK(!httpParseImfFixdate("bad").has_value());                          // wrong length
    RUVIA_CHECK(!httpParseImfFixdate("Foo, 06 Nov 1994 08:49:37 GMT").has_value());  // bad day-name
    RUVIA_CHECK(!httpParseImfFixdate("sun, 06 Nov 1994 08:49:37 GMT").has_value());  // case-sensitive
    RUVIA_CHECK(!httpParseImfFixdate("Sun  06 Nov 1994 08:49:37 GMT").has_value());  // bad separators
    RUVIA_CHECK(!httpParseImfFixdate("Sun, 06 Xxx 1994 08:49:37 GMT").has_value());  // bad month
    RUVIA_CHECK(!httpParseImfFixdate("Sun, 32 Nov 1994 08:49:37 GMT").has_value());  // day out of range
    RUVIA_CHECK(!httpParseImfFixdate("Sun, 06 Nov 1994 08:49:37 UTC").has_value());  // not GMT
}

RUVIA_TEST(http_parse_http_date_accepts_all_three_formats) {
    using ruvia::detail::httpParseHttpDate;
    // RFC 7231 section 7.1.1.1: a recipient MUST accept all three date formats.
    // The one canonical instant, written each way, resolves to the same second.
    constexpr std::time_t canonical{784111777};
    RUVIA_CHECK_EQ(httpParseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT").value_or(-1), canonical);   // IMF-fixdate
    RUVIA_CHECK_EQ(httpParseHttpDate("Sunday, 06-Nov-94 08:49:37 GMT").value_or(-1), canonical);  // RFC 850
    RUVIA_CHECK_EQ(httpParseHttpDate("Sun Nov  6 08:49:37 1994").value_or(-1), canonical);        // asctime

    // asctime with a two-digit day is not space-padded.
    RUVIA_CHECK(httpParseHttpDate("Sun Nov 16 08:49:37 1994").has_value());

    // RFC 9110 uses a rolling 50-year pivot, not the POSIX 68/69 split.
    using ruvia::detail::httpResolveRfc850Year;
    RUVIA_CHECK_EQ(httpResolveRfc850Year(70, 2026), 2070);
    RUVIA_CHECK_EQ(httpResolveRfc850Year(76, 2026), 2076);
    RUVIA_CHECK_EQ(httpResolveRfc850Year(77, 2026), 1977);
    RUVIA_CHECK_EQ(httpResolveRfc850Year(99, 2090), 2099);
    RUVIA_CHECK_EQ(httpResolveRfc850Year(0, 2090), 2000);

    // A malformed instance of each obsolete format is rejected, not silently
    // coerced through the shared assembler.
    RUVIA_CHECK(!httpParseHttpDate("Funday, 06-Nov-94 08:49:37 GMT").has_value());  // bad long day-name
    RUVIA_CHECK(!httpParseHttpDate("Foo Nov  6 08:49:37 1994").has_value());       // bad short day-name
    RUVIA_CHECK(!httpParseHttpDate("Sunday, 06-Xxx-94 08:49:37 GMT").has_value());  // bad month (RFC 850)
    RUVIA_CHECK(!httpParseHttpDate("Sunday, 06-Nov-94 08:49:37 UTC").has_value());  // not GMT (RFC 850)
    RUVIA_CHECK(!httpParseHttpDate("Sun Xxx  6 08:49:37 1994").has_value());        // bad month (asctime)
    RUVIA_CHECK(!httpParseHttpDate("garbage").has_value());
}
