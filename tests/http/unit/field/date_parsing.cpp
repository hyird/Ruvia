#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/http/detail/field/HttpDate.h"
#include "ruvia/http/detail/field/HttpImfFixdate.h"
#include "ruvia/http/detail/server/HttpDateCache.h"

#include "test_harness.h"

namespace {

std::string formatDate(std::time_t time) {
    const auto out = ruvia::detail::httpFormatDate(time).value();
    return std::string(out.data(), out.size());
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
    RUVIA_CHECK(!httpParseImfFixdate("bad").has_value());                            // wrong length
    RUVIA_CHECK(!httpParseImfFixdate("Foo, 06 Nov 1994 08:49:37 GMT").has_value());  // bad day-name
    RUVIA_CHECK(
        !httpParseImfFixdate("sun, 06 Nov 1994 08:49:37 GMT").has_value());  // case-sensitive
    RUVIA_CHECK(
        !httpParseImfFixdate("Sun  06 Nov 1994 08:49:37 GMT").has_value());          // bad separators
    RUVIA_CHECK(!httpParseImfFixdate("Sun, 06 Xxx 1994 08:49:37 GMT").has_value());  // bad month
    RUVIA_CHECK(
        !httpParseImfFixdate("Sun, 32 Nov 1994 08:49:37 GMT").has_value());          // day out of range
    RUVIA_CHECK(!httpParseImfFixdate("Sun, 06 Nov 1994 08:49:37 UTC").has_value());  // not GMT
}

RUVIA_TEST(http_parse_http_date_accepts_all_three_formats) {
    using ruvia::detail::httpParseHttpDate;
    // RFC 7231 section 7.1.1.1: a recipient MUST accept all three date formats.
    // The one canonical instant, written each way, resolves to the same second.
    constexpr std::time_t canonical{784111777};
    RUVIA_CHECK_EQ(
        httpParseHttpDate("Sun, 06 Nov 1994 08:49:37 GMT").value_or(-1), canonical);  // IMF-fixdate
    RUVIA_CHECK_EQ(
        httpParseHttpDate("Sunday, 06-Nov-94 08:49:37 GMT").value_or(-1), canonical);  // RFC 850
    RUVIA_CHECK_EQ(
        httpParseHttpDate("Sun Nov  6 08:49:37 1994").value_or(-1), canonical);  // asctime

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
    RUVIA_CHECK(
        !httpParseHttpDate("Funday, 06-Nov-94 08:49:37 GMT").has_value());    // bad long day-name
    RUVIA_CHECK(!httpParseHttpDate("Foo Nov  6 08:49:37 1994").has_value());  // bad short day-name
    RUVIA_CHECK(
        !httpParseHttpDate("Sunday, 06-Xxx-94 08:49:37 GMT").has_value());  // bad month (RFC 850)
    RUVIA_CHECK(
        !httpParseHttpDate("Sunday, 06-Nov-94 08:49:37 UTC").has_value());    // not GMT (RFC 850)
    RUVIA_CHECK(!httpParseHttpDate("Sun Xxx  6 08:49:37 1994").has_value());  // bad month (asctime)
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
    RUVIA_CHECK(
        !httpParseImfFixdate("Thu, 01 Jan 1970 00:00:00").has_value());  // wrong length / no GMT
    RUVIA_CHECK(
        !httpParseImfFixdate("Thu, 01 Jan 1970 00:00:00 UTC").has_value());          // zone must be GMT
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jon 1970 00:00:00 GMT").has_value());  // bad month
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jan 1970 25:00:00 GMT").has_value());  // hour > 23
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 00 Jan 1970 00:00:00 GMT").has_value());  // day < 1
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 32 Jan 1970 00:00:00 GMT").has_value());  // day > 31
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jan 1970 00:60:00 GMT").has_value());  // minute > 59
    RUVIA_CHECK(
        !httpParseImfFixdate("Thu; 01 Jan 1970 00:00:00 GMT").has_value());  // wrong separator

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
    RUVIA_CHECK(
        httpParseImfFixdate("Thu, 01 Jan 1970 23:59:60 GMT").has_value());           // leap second allowed
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jan 1970 23:59:61 GMT").has_value());  // 61 rejected
}

RUVIA_TEST(http_date_rejects_nonexistent_calendar_days) {
    using ruvia::detail::httpParseAsctimeDate;
    using ruvia::detail::httpParseImfFixdate;
    using ruvia::detail::httpParseRfc850Date;

    // The civil-date conversion must not normalize impossible dates into the
    // following month. All three HTTP-date syntaxes share this validation.
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 31 Apr 1970 00:00:00 GMT").has_value());
    RUVIA_CHECK(!httpParseRfc850Date("Thursday, 31-Apr-70 00:00:00 GMT").has_value());
    RUVIA_CHECK(!httpParseAsctimeDate("Thu Apr 31 00:00:00 1970").has_value());

    RUVIA_CHECK(!httpParseImfFixdate("Mon, 29 Feb 1900 00:00:00 GMT").has_value());
    RUVIA_CHECK(httpParseImfFixdate("Tue, 29 Feb 2000 00:00:00 GMT").has_value());
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
    const std::int64_t samples[] = {
        -62167219200LL, -11644473600LL, -315619200, -1,
        0, 1, 59, 3661, 86400, 784111777, 1000000000, 1600000000, 2000000000, 2147483647,
        32535216000LL, 253402300799LL};
    for (const auto sample : samples) {
        if (!std::in_range<std::time_t>(sample)) {
            continue;
        }
        const auto formatted = formatDate(static_cast<std::time_t>(sample));
        RUVIA_CHECK_EQ(formatted.size(), std::size_t{29});
        const auto parsed = httpParseImfFixdate(formatted);
        RUVIA_CHECK(parsed.has_value());
        if (parsed) {
            RUVIA_CHECK_EQ(*parsed, sample);
        }
    }
}

RUVIA_TEST(http_date_conversion_rejects_unrepresentable_years) {
    for (const auto time : {std::int64_t{-62167219201LL}, std::int64_t{253402300800LL},
             std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
        if (!std::in_range<std::time_t>(time)) {
            continue;
        }
        const auto date = ruvia::detail::httpFormatDate(static_cast<std::time_t>(time));
        RUVIA_CHECK(!date);
        if (!date) {
            RUVIA_CHECK(date.error() == ruvia::detail::HttpDateFormatError::kOutOfRange);
        }
    }
    RUVIA_CHECK_EQ(formatDate(-315619200), std::string("Fri, 01 Jan 1960 00:00:00 GMT"));
    RUVIA_CHECK_EQ(formatDate(-1), std::string("Wed, 31 Dec 1969 23:59:59 GMT"));
    if (std::in_range<std::time_t>(253402300799LL)) {
        RUVIA_CHECK_EQ(formatDate(static_cast<std::time_t>(253402300799LL)), std::string("Fri, 31 Dec 9999 23:59:59 GMT"));
    }
}

RUVIA_TEST(http_date_cache_omits_unavailable_dates_and_recovers) {
    using ruvia::detail::cachedDateHeader;
    using ruvia::detail::cachedDateValue;
    RUVIA_CHECK(!cachedDateHeader(0).empty());
    for (const auto value : {std::int64_t{-1}, std::numeric_limits<std::int64_t>::max(),
             std::numeric_limits<std::int64_t>::min()}) {
        if (!std::in_range<std::time_t>(value)) {
            continue;
        }
        const auto time = static_cast<std::time_t>(value);
        RUVIA_CHECK(cachedDateHeader(time).empty());
        RUVIA_CHECK(cachedDateValue(time).empty());
        RUVIA_CHECK(cachedDateHeader(time).empty());
    }
    RUVIA_CHECK_EQ(cachedDateValue(-2), std::string_view("Wed, 31 Dec 1969 23:59:58 GMT"));
    RUVIA_CHECK_EQ(cachedDateHeader(0), std::string_view("Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n"));
    RUVIA_CHECK_EQ(cachedDateValue(0), std::string_view("Thu, 01 Jan 1970 00:00:00 GMT"));
}

RUVIA_TEST(imf_fixdate_format_known_vector) {
    const auto date = ruvia::detail::httpFormatDate(784111777).value();
    RUVIA_CHECK_EQ(date.size(), ruvia::detail::kImfFixdateSize);
    RUVIA_CHECK_EQ(std::string(date.data(), date.size()), std::string("Sun, 06 Nov 1994 08:49:37 GMT"));
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
