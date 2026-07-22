#include "test_harness.h"

#include "ruvia/http/detail/field/HttpDate.h"
#include "ruvia/http/detail/server/HttpDateCache.h"
#include "ruvia/http/detail/field/HttpImfFixdate.h"

#include <ctime>
#include <memory_resource>
#include <string>

namespace {

std::string formatDate(std::time_t time) {
    const auto out = ruvia::detail::httpFormatDate(std::pmr::get_default_resource(), time);
    return std::string(out.data(), out.size());
}

}  // namespace


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

RUVIA_TEST(imf_fixdate_parses_known_dates) {
    using ruvia::detail::httpParseImfFixdate;
    const auto epoch = httpParseImfFixdate("Thu, 01 Jan 1970 00:00:00 GMT");
    RUVIA_CHECK(epoch.has_value());
    if (epoch) {
        RUVIA_CHECK_EQ(*epoch, std::time_t{0});
    }
    const auto nextDay = httpParseImfFixdate("Fri, 02 Jan 1970 00:00:00 GMT");
    RUVIA_CHECK(nextDay.has_value());
    if (nextDay) {
        RUVIA_CHECK_EQ(*nextDay, std::time_t{86400});
    }
    // A later date must compare strictly greater (monotonic).
    const auto later = httpParseImfFixdate("Sun, 06 Nov 1994 08:49:37 GMT");
    RUVIA_CHECK(later.has_value());
    if (later && epoch) {
        RUVIA_CHECK(*later > *epoch);
    }
    // Parsing is deterministic.
    RUVIA_CHECK(httpParseImfFixdate("Sun, 06 Nov 1994 08:49:37 GMT") == later);
}

RUVIA_TEST(imf_fixdate_rejects_malformed) {
    using ruvia::detail::httpParseImfFixdate;
    RUVIA_CHECK(!httpParseImfFixdate("").has_value());
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jan 1970 00:00:00").has_value());       // wrong length / no GMT
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jan 1970 00:00:00 UTC").has_value());   // zone must be GMT
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jon 1970 00:00:00 GMT").has_value());   // bad month
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jan 1970 25:00:00 GMT").has_value());   // hour > 23
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 00 Jan 1970 00:00:00 GMT").has_value());   // day < 1
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 32 Jan 1970 00:00:00 GMT").has_value());   // day > 31
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jan 1970 00:60:00 GMT").has_value());   // minute > 59
    RUVIA_CHECK(!httpParseImfFixdate("Thu; 01 Jan 1970 00:00:00 GMT").has_value());   // wrong separator

    // httpParseImfFixdate is the strict IMF-fixdate-only component: it MUST reject the
    // two obsolete HTTP-date formats. (RFC 9110 §5.6.7 requires a recipient to accept
    // all three formats; that is satisfied by the composite httpParseHttpDate, which
    // falls back to httpParseRfc850Date / httpParseAsctimeDate -- the conditional-request
    // call sites use that composite, not this component.) Pinning the component's
    // rejection keeps its fixed-length invariant honest.
    RUVIA_CHECK(!httpParseImfFixdate("Sunday, 06-Nov-94 08:49:37 GMT").has_value());  // RFC 850
    RUVIA_CHECK(!httpParseImfFixdate("Sun Nov  6 08:49:37 1994").has_value());        // asctime()
}

RUVIA_TEST(imf_fixdate_leap_second_boundary) {
    using ruvia::detail::httpParseImfFixdate;
    RUVIA_CHECK(httpParseImfFixdate("Thu, 01 Jan 1970 23:59:60 GMT").has_value());   // leap second allowed
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jan 1970 23:59:61 GMT").has_value());  // 61 rejected
}

RUVIA_TEST(http_date_rejects_nonexistent_calendar_days) {
    using ruvia::detail::httpParseAsctimeDate;
    using ruvia::detail::httpParseImfFixdate;
    using ruvia::detail::httpParseRfc850Date;

    // The civil-date conversion must not normalize impossible dates into the
    // following month. All three HTTP-date syntaxes share this validation.
    RUVIA_CHECK(!httpParseImfFixdate(
        "Thu, 31 Apr 1970 00:00:00 GMT").has_value());
    RUVIA_CHECK(!httpParseRfc850Date(
        "Thursday, 31-Apr-70 00:00:00 GMT").has_value());
    RUVIA_CHECK(!httpParseAsctimeDate(
        "Thu Apr 31 00:00:00 1970").has_value());

    RUVIA_CHECK(!httpParseImfFixdate(
        "Mon, 29 Feb 1900 00:00:00 GMT").has_value());
    RUVIA_CHECK(httpParseImfFixdate(
        "Tue, 29 Feb 2000 00:00:00 GMT").has_value());
}

RUVIA_TEST(http_format_date_known_vectors) {
    RUVIA_CHECK_EQ(formatDate(0), std::string("Thu, 01 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(formatDate(86400), std::string("Fri, 02 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(formatDate(784111777), std::string("Sun, 06 Nov 1994 08:49:37 GMT"));
    // The parser ignores the weekday token, so only a known-vector check catches a
    // wrong entry in the writer's weekday table. Epoch 0 is a Thursday; the days
    // that follow cover the remaining weekday names (Sat/Mon/Tue/Wed).
    RUVIA_CHECK_EQ(formatDate(172800), std::string("Sat, 03 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(formatDate(345600), std::string("Mon, 05 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(formatDate(432000), std::string("Tue, 06 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(formatDate(518400), std::string("Wed, 07 Jan 1970 00:00:00 GMT"));
}

RUVIA_TEST(http_format_date_round_trips_with_parse) {
    using ruvia::detail::httpParseImfFixdate;
    const std::time_t samples[] = {
        0, 1, 59, 3661, 86400, 784111777, 1000000000, 1600000000, 2000000000, 2147483647};
    for (const auto sample : samples) {
        const auto formatted = formatDate(sample);
        RUVIA_CHECK_EQ(formatted.size(), std::size_t{29});
        const auto parsed = httpParseImfFixdate(formatted);
        RUVIA_CHECK(parsed.has_value());
        if (parsed) {
            RUVIA_CHECK_EQ(*parsed, sample);
        }
    }
}

RUVIA_TEST(imf_fixdate_writer_known_vector) {
    std::tm utc{};
    utc.tm_wday = 0;   // Sunday
    utc.tm_mday = 6;
    utc.tm_mon = 10;   // November
    utc.tm_year = 94;  // 1994
    utc.tm_hour = 8;
    utc.tm_min = 49;
    utc.tm_sec = 37;
    char buffer[ruvia::detail::kImfFixdateSize];
    const auto written = ruvia::detail::httpWriteImfFixdate(buffer, utc);
    RUVIA_CHECK_EQ(written, ruvia::detail::kImfFixdateSize);
    RUVIA_CHECK_EQ(std::string(buffer, written), std::string("Sun, 06 Nov 1994 08:49:37 GMT"));
}

RUVIA_TEST(imf_fixdate_writer_clamps_out_of_range_indices) {
    // A corrupt tm must not index the day/month tables out of bounds.
    std::tm utc{};
    utc.tm_wday = 99;
    utc.tm_mon = 99;
    utc.tm_mday = 1;
    utc.tm_year = 70;
    char buffer[ruvia::detail::kImfFixdateSize];
    const auto written = ruvia::detail::httpWriteImfFixdate(buffer, utc);
    RUVIA_CHECK_EQ(written, ruvia::detail::kImfFixdateSize);
    RUVIA_CHECK_EQ(std::string(buffer, 3), std::string("Sun"));       // wday 99 -> 0
    RUVIA_CHECK_EQ(std::string(buffer + 8, 3), std::string("Jan"));   // mon 99 -> 0
}

RUVIA_TEST(cached_date_header_framing_and_validity) {
    using ruvia::detail::cachedDateHeader;
    using ruvia::detail::cachedDateValue;
    using ruvia::detail::httpParseImfFixdate;

    const std::string header(cachedDateHeader());
    RUVIA_CHECK_EQ(header.size(), std::size_t{37});  // "Date: " (6) + date (29) + CRLF (2)
    RUVIA_CHECK(header.starts_with("Date: "));
    RUVIA_CHECK(std::string_view(header).ends_with("\r\n"));

    // The 29-char value must parse as a valid IMF-fixdate; a localized or
    // malformed date (the pre-fix strftime %a/%b risk) would fail to parse.
    const std::string_view valuePart = std::string_view(header).substr(6, 29);
    const auto parsedFromHeader = httpParseImfFixdate(valuePart);
    RUVIA_CHECK(parsedFromHeader.has_value());
    if (parsedFromHeader) {
        const auto now = std::time(nullptr);
        RUVIA_CHECK(*parsedFromHeader <= now && now - *parsedFromHeader < 3);
    }

    // The bare value accessor (HPACK :date) is likewise a valid 29-char date.
    const std::string value(cachedDateValue());
    RUVIA_CHECK_EQ(value.size(), std::size_t{29});
    RUVIA_CHECK(httpParseImfFixdate(value).has_value());
}
