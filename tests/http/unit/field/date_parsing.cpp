#include <cstddef>
#include <cstdint>
#include <ctime>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http_date.h"

#include "field/http_date.h"
#include "field/http_imf_fixdate.h"
#include "server/http_date_cache.h"
#include "test_harness.h"

namespace {

std::string format_date(std::time_t time) {
    const auto out = ruvia::format_http_date(time).value();
    return std::string(out.data(), out.size());
}

}  // namespace

// Reading an HTTP date: IMF-fixdate and the two obsolete formats a recipient must still accept.

RUVIA_TEST(http_month_index_lookup) {
    using ruvia::detail::http_month_index;
    RUVIA_CHECK_EQ(http_month_index("Jan"), 1);
    RUVIA_CHECK_EQ(http_month_index("Feb"), 2);
    RUVIA_CHECK_EQ(http_month_index("Nov"), 11);
    RUVIA_CHECK_EQ(http_month_index("Dec"), 12);
    // Unknown, empty, and wrong-case names are rejected (0).
    RUVIA_CHECK_EQ(http_month_index("Xyz"), 0);
    RUVIA_CHECK_EQ(http_month_index(""), 0);
    RUVIA_CHECK_EQ(http_month_index("jan"), 0);
}

RUVIA_TEST(http_parse_fixed_digits) {
    using ruvia::detail::http_parse_fixed_digits;
    RUVIA_CHECK_EQ(http_parse_fixed_digits("07").value_or(-1), 7);
    RUVIA_CHECK_EQ(http_parse_fixed_digits("2024").value_or(-1), 2024);
    RUVIA_CHECK_EQ(http_parse_fixed_digits("00").value_or(-1), 0);
    // Empty and any non-digit byte are rejected.
    RUVIA_CHECK(!http_parse_fixed_digits("").has_value());
    RUVIA_CHECK(!http_parse_fixed_digits("1a").has_value());
    RUVIA_CHECK(!http_parse_fixed_digits(" 7").has_value());
}

RUVIA_TEST(http_days_from_civil_epoch) {
    using ruvia::detail::http_days_from_civil;
    // Days relative to the Unix epoch (Howard Hinnant's algorithm).
    RUVIA_CHECK_EQ(http_days_from_civil(1970, 1, 1), std::int64_t{0});
    RUVIA_CHECK_EQ(http_days_from_civil(1970, 1, 2), std::int64_t{1});
    RUVIA_CHECK_EQ(http_days_from_civil(1969, 12, 31), std::int64_t{-1});
    RUVIA_CHECK_EQ(http_days_from_civil(2000, 1, 1), std::int64_t{10957});
    RUVIA_CHECK_EQ(http_days_from_civil(1994, 11, 6), std::int64_t{9075});
}

RUVIA_TEST(http_parse_imf_fixdate) {
    using ruvia::detail::http_parse_imf_fixdate;
    // The canonical RFC 7231 example resolves to its known epoch second.
    const auto canonical = http_parse_imf_fixdate("Sun, 06 Nov 1994 08:49:37 GMT");
    RUVIA_CHECK(canonical.has_value());
    RUVIA_CHECK_EQ(*canonical, std::time_t{784111777});
    // The Unix epoch itself.
    const auto epoch = http_parse_imf_fixdate("Thu, 01 Jan 1970 00:00:00 GMT");
    RUVIA_CHECK(epoch.has_value());
    RUVIA_CHECK_EQ(*epoch, std::time_t{0});
    // A leap second (60) is accepted.
    RUVIA_CHECK(http_parse_imf_fixdate("Sun, 06 Nov 1994 08:49:60 GMT").has_value());

    // Malformed inputs are rejected.
    RUVIA_CHECK(!http_parse_imf_fixdate("bad").has_value());                            // wrong length
    RUVIA_CHECK(!http_parse_imf_fixdate("Foo, 06 Nov 1994 08:49:37 GMT").has_value());  // bad day-name
    RUVIA_CHECK(
        !http_parse_imf_fixdate("sun, 06 Nov 1994 08:49:37 GMT").has_value());  // case-sensitive
    RUVIA_CHECK(
        !http_parse_imf_fixdate("Sun  06 Nov 1994 08:49:37 GMT").has_value());          // bad separators
    RUVIA_CHECK(!http_parse_imf_fixdate("Sun, 06 Xxx 1994 08:49:37 GMT").has_value());  // bad month
    RUVIA_CHECK(
        !http_parse_imf_fixdate("Sun, 32 Nov 1994 08:49:37 GMT").has_value());          // day out of range
    RUVIA_CHECK(!http_parse_imf_fixdate("Sun, 06 Nov 1994 08:49:37 UTC").has_value());  // not GMT
}

RUVIA_TEST(http_parse_http_date_accepts_all_three_formats) {
    using ruvia::detail::http_parse_http_date;
    // RFC 7231 section 7.1.1.1: a recipient MUST accept all three date formats.
    // The one canonical instant, written each way, resolves to the same second.
    constexpr std::time_t canonical{784111777};
    RUVIA_CHECK_EQ(
        http_parse_http_date("Sun, 06 Nov 1994 08:49:37 GMT").value_or(-1), canonical);  // IMF-fixdate
    RUVIA_CHECK_EQ(
        http_parse_http_date("Sunday, 06-Nov-94 08:49:37 GMT").value_or(-1), canonical);  // RFC 850
    RUVIA_CHECK_EQ(
        http_parse_http_date("Sun Nov  6 08:49:37 1994").value_or(-1), canonical);  // asctime

    // asctime with a two-digit day is not space-padded.
    RUVIA_CHECK(http_parse_http_date("Sun Nov 16 08:49:37 1994").has_value());

    // RFC 9110 uses a rolling 50-year pivot, not the POSIX 68/69 split.
    using ruvia::detail::http_resolve_rfc850_year;
    RUVIA_CHECK_EQ(http_resolve_rfc850_year(70, 2026), 2070);
    RUVIA_CHECK_EQ(http_resolve_rfc850_year(76, 2026), 2076);
    RUVIA_CHECK_EQ(http_resolve_rfc850_year(77, 2026), 1977);
    RUVIA_CHECK_EQ(http_resolve_rfc850_year(99, 2090), 2099);
    RUVIA_CHECK_EQ(http_resolve_rfc850_year(0, 2090), 2000);

    // A malformed instance of each obsolete format is rejected, not silently
    // coerced through the shared assembler.
    RUVIA_CHECK(
        !http_parse_http_date("Funday, 06-Nov-94 08:49:37 GMT").has_value());    // bad long day-name
    RUVIA_CHECK(!http_parse_http_date("Foo Nov  6 08:49:37 1994").has_value());  // bad short day-name
    RUVIA_CHECK(
        !http_parse_http_date("Sunday, 06-Xxx-94 08:49:37 GMT").has_value());  // bad month (RFC 850)
    RUVIA_CHECK(
        !http_parse_http_date("Sunday, 06-Nov-94 08:49:37 UTC").has_value());    // not GMT (RFC 850)
    RUVIA_CHECK(!http_parse_http_date("Sun Xxx  6 08:49:37 1994").has_value());  // bad month (asctime)
    RUVIA_CHECK(!http_parse_http_date("garbage").has_value());
}

RUVIA_TEST(http_rfc850_date_keeps_the_rolling_century_rule) {
    using ruvia::detail::http_parse_imf_fixdate;
    using ruvia::detail::http_parse_rfc850_date;
    struct date_case final {
        std::string_view reference_;
        std::string_view field_;
        std::string_view expected_;
    };
    constexpr date_case cases[] = {
        {"Thu, 01 Jan 2026 00:00:00 GMT", "Wednesday, 01-Jan-70 00:00:00 GMT", "Wed, 01 Jan 2070 00:00:00 GMT"},
        {"Thu, 01 Jan 2026 00:00:00 GMT", "Wednesday, 01-Jan-76 00:00:00 GMT", "Wed, 01 Jan 2076 00:00:00 GMT"},
        {"Thu, 01 Jan 2026 00:00:00 GMT", "Saturday, 01-Jan-77 00:00:00 GMT", "Sat, 01 Jan 1977 00:00:00 GMT"},
        {"Sun, 01 Jan 2090 00:00:00 GMT", "Thursday, 01-Jan-99 00:00:00 GMT", "Thu, 01 Jan 2099 00:00:00 GMT"},
        {"Sun, 01 Jan 2090 00:00:00 GMT", "Saturday, 01-Jan-00 00:00:00 GMT", "Sat, 01 Jan 2000 00:00:00 GMT"},
    };
    for (const auto& item : cases) {
        const auto reference = http_parse_imf_fixdate(item.reference_);
        // A platform with a narrower time_t cannot supply every reference.
        if (reference) {
            RUVIA_CHECK(http_parse_rfc850_date(item.field_, *reference) == http_parse_imf_fixdate(item.expected_));
        }
    }
}

RUVIA_TEST(http_rfc850_date_uses_the_complete_fifty_year_boundary) {
    using ruvia::detail::http_parse_imf_fixdate;
    using ruvia::detail::http_parse_rfc850_date;
    const auto reference = http_parse_imf_fixdate("Thu, 15 Jan 2026 12:34:56 GMT");
    RUVIA_CHECK(reference.has_value());
    if (!reference) {
        return;
    }
    struct date_case final {
        std::string_view field_;
        std::string_view expected_;
    };
    constexpr date_case cases[] = {
        {"Wednesday, 14-Jan-76 23:59:59 GMT", "Tue, 14 Jan 2076 23:59:59 GMT"},
        {"Wednesday, 15-Jan-76 12:34:55 GMT", "Wed, 15 Jan 2076 12:34:55 GMT"},
        {"Wednesday, 15-Jan-76 12:34:56 GMT", "Wed, 15 Jan 2076 12:34:56 GMT"},
        {"Thursday, 15-Jan-76 12:34:57 GMT", "Thu, 15 Jan 1976 12:34:57 GMT"},
        {"Thursday, 15-Jan-76 12:35:00 GMT", "Thu, 15 Jan 1976 12:35:00 GMT"},
        {"Thursday, 15-Jan-76 13:00:00 GMT", "Thu, 15 Jan 1976 13:00:00 GMT"},
        {"Friday, 16-Jan-76 00:00:00 GMT", "Fri, 16 Jan 1976 00:00:00 GMT"},
        {"Sunday, 01-Feb-76 00:00:00 GMT", "Sun, 01 Feb 1976 00:00:00 GMT"},
        {"Friday, 31-Dec-76 00:00:00 GMT", "Fri, 31 Dec 1976 00:00:00 GMT"},
    };
    for (const auto& item : cases) {
        RUVIA_CHECK(http_parse_rfc850_date(item.field_, *reference) == http_parse_imf_fixdate(item.expected_));
    }
}

RUVIA_TEST(http_rfc850_date_accepts_an_explicit_pre_epoch_reference) {
    using ruvia::detail::http_parse_imf_fixdate;
    using ruvia::detail::http_parse_rfc850_date;
    const auto reference = http_parse_imf_fixdate("Wed, 31 Dec 1969 23:59:59 GMT");
    if (reference) {
        RUVIA_CHECK(http_parse_rfc850_date("Wednesday, 31-Dec-69 23:59:59 GMT", *reference) == reference);
    }
}

RUVIA_TEST(imf_fixdate_parses_known_dates) {
    using ruvia::detail::http_parse_imf_fixdate;
    const auto epoch = http_parse_imf_fixdate("Thu, 01 Jan 1970 00:00:00 GMT");
    RUVIA_CHECK(epoch.has_value());
    if (epoch) {
        RUVIA_CHECK_EQ(*epoch, std::time_t{0});
    }
    const auto next_day = http_parse_imf_fixdate("Fri, 02 Jan 1970 00:00:00 GMT");
    RUVIA_CHECK(next_day.has_value());
    if (next_day) {
        RUVIA_CHECK_EQ(*next_day, std::time_t{86400});
    }
    // A later date must compare strictly greater (monotonic).
    const auto later = http_parse_imf_fixdate("Sun, 06 Nov 1994 08:49:37 GMT");
    RUVIA_CHECK(later.has_value());
    if (later && epoch) {
        RUVIA_CHECK(*later > *epoch);
    }
    // Parsing is deterministic.
    RUVIA_CHECK(http_parse_imf_fixdate("Sun, 06 Nov 1994 08:49:37 GMT") == later);
}

RUVIA_TEST(imf_fixdate_rejects_malformed) {
    using ruvia::detail::http_parse_imf_fixdate;
    RUVIA_CHECK(!http_parse_imf_fixdate("").has_value());
    RUVIA_CHECK(
        !http_parse_imf_fixdate("Thu, 01 Jan 1970 00:00:00").has_value());  // wrong length / no GMT
    RUVIA_CHECK(
        !http_parse_imf_fixdate("Thu, 01 Jan 1970 00:00:00 UTC").has_value());          // zone must be GMT
    RUVIA_CHECK(!http_parse_imf_fixdate("Thu, 01 Jon 1970 00:00:00 GMT").has_value());  // bad month
    RUVIA_CHECK(!http_parse_imf_fixdate("Thu, 01 Jan 1970 25:00:00 GMT").has_value());  // hour > 23
    RUVIA_CHECK(!http_parse_imf_fixdate("Thu, 00 Jan 1970 00:00:00 GMT").has_value());  // day < 1
    RUVIA_CHECK(!http_parse_imf_fixdate("Thu, 32 Jan 1970 00:00:00 GMT").has_value());  // day > 31
    RUVIA_CHECK(!http_parse_imf_fixdate("Thu, 01 Jan 1970 00:60:00 GMT").has_value());  // minute > 59
    RUVIA_CHECK(
        !http_parse_imf_fixdate("Thu; 01 Jan 1970 00:00:00 GMT").has_value());  // wrong separator

    // http_parse_imf_fixdate is the strict IMF-fixdate-only component: it MUST reject the
    // two obsolete HTTP-date formats. (RFC 9110 §5.6.7 requires a recipient to accept
    // all three formats; that is satisfied by the composite http_parse_http_date, which
    // falls back to http_parse_rfc850_date / http_parse_asctime_date -- the conditional-request
    // call sites use that composite, not this component.) Pinning the component's
    // rejection keeps its fixed-length invariant honest.
    RUVIA_CHECK(!http_parse_imf_fixdate("Sunday, 06-Nov-94 08:49:37 GMT").has_value());  // RFC 850
    RUVIA_CHECK(!http_parse_imf_fixdate("Sun Nov  6 08:49:37 1994").has_value());        // asctime()
}

RUVIA_TEST(imf_fixdate_leap_second_boundary) {
    using ruvia::detail::http_parse_imf_fixdate;
    RUVIA_CHECK(
        http_parse_imf_fixdate("Thu, 01 Jan 1970 23:59:60 GMT").has_value());           // leap second allowed
    RUVIA_CHECK(!http_parse_imf_fixdate("Thu, 01 Jan 1970 23:59:61 GMT").has_value());  // 61 rejected
}

RUVIA_TEST(http_date_rejects_nonexistent_calendar_days) {
    using ruvia::detail::http_parse_asctime_date;
    using ruvia::detail::http_parse_imf_fixdate;
    using ruvia::detail::http_parse_rfc850_date;

    // The civil-date conversion must not normalize impossible dates into the
    // following month. All three HTTP-date syntaxes share this validation.
    RUVIA_CHECK(!http_parse_imf_fixdate("Thu, 31 Apr 1970 00:00:00 GMT").has_value());
    RUVIA_CHECK(!http_parse_rfc850_date("Thursday, 31-Apr-70 00:00:00 GMT").has_value());
    RUVIA_CHECK(!http_parse_asctime_date("Thu Apr 31 00:00:00 1970").has_value());

    RUVIA_CHECK(!http_parse_imf_fixdate("Mon, 29 Feb 1900 00:00:00 GMT").has_value());
    RUVIA_CHECK(http_parse_imf_fixdate("Tue, 29 Feb 2000 00:00:00 GMT").has_value());
}

RUVIA_TEST(http_format_date_known_vectors) {
    RUVIA_CHECK_EQ(format_date(0), std::string("Thu, 01 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(format_date(86400), std::string("Fri, 02 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(format_date(784111777), std::string("Sun, 06 Nov 1994 08:49:37 GMT"));
    // The parser ignores the weekday token, so only a known-vector check catches a
    // wrong entry in the writer's weekday table. Epoch 0 is a Thursday; the days
    // that follow cover the remaining weekday names (Sat/Mon/Tue/Wed).
    RUVIA_CHECK_EQ(format_date(172800), std::string("Sat, 03 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(format_date(345600), std::string("Mon, 05 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(format_date(432000), std::string("Tue, 06 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(format_date(518400), std::string("Wed, 07 Jan 1970 00:00:00 GMT"));
}

RUVIA_TEST(http_format_date_round_trips_with_parse) {
    using ruvia::detail::http_parse_imf_fixdate;
    const std::int64_t samples[] = {
        -62167219200LL, -11644473600LL, -315619200, -1,
        0, 1, 59, 3661, 86400, 784111777, 1000000000, 1600000000, 2000000000, 2147483647,
        32535216000LL, 253402300799LL};
    for (const auto sample : samples) {
        if (!std::in_range<std::time_t>(sample)) {
            continue;
        }
        const auto formatted = format_date(static_cast<std::time_t>(sample));
        RUVIA_CHECK_EQ(formatted.size(), std::size_t{29});
        const auto parsed_value = http_parse_imf_fixdate(formatted);
        RUVIA_CHECK(parsed_value.has_value());
        if (parsed_value) {
            RUVIA_CHECK_EQ(*parsed_value, sample);
        }
    }
}

RUVIA_TEST(http_date_conversion_rejects_unrepresentable_years) {
    for (const auto time : {std::int64_t{-62167219201LL}, std::int64_t{253402300800LL},
             std::numeric_limits<std::int64_t>::min(), std::numeric_limits<std::int64_t>::max()}) {
        if (!std::in_range<std::time_t>(time)) {
            continue;
        }
        const auto date = ruvia::detail::http_format_date(static_cast<std::time_t>(time));
        RUVIA_CHECK(!ruvia::format_http_date(static_cast<std::time_t>(time)));
        RUVIA_CHECK((date.index() != 0));
        if ((date.index() != 0)) {
            RUVIA_CHECK(std::get<1>(date) == ruvia::detail::http_date_format_error::out_of_range);
        }
    }
    RUVIA_CHECK_EQ(format_date(-315619200), std::string("Fri, 01 Jan 1960 00:00:00 GMT"));
    RUVIA_CHECK_EQ(format_date(-1), std::string("Wed, 31 Dec 1969 23:59:59 GMT"));
    if (std::in_range<std::time_t>(253402300799LL)) {
        RUVIA_CHECK_EQ(format_date(static_cast<std::time_t>(253402300799LL)), std::string("Fri, 31 Dec 9999 23:59:59 GMT"));
    }
}

RUVIA_TEST(http_date_cache_omits_unavailable_dates_and_recovers) {
    using ruvia::detail::cached_date_header;
    using ruvia::detail::cached_date_value;
    RUVIA_CHECK(!cached_date_header(0).empty());
    for (const auto value : {std::int64_t{-1}, std::numeric_limits<std::int64_t>::max(),
             std::numeric_limits<std::int64_t>::min()}) {
        if (!std::in_range<std::time_t>(value)) {
            continue;
        }
        const auto time = static_cast<std::time_t>(value);
        RUVIA_CHECK(cached_date_header(time).empty());
        RUVIA_CHECK(cached_date_value(time).empty());
        RUVIA_CHECK(cached_date_header(time).empty());
    }
    RUVIA_CHECK_EQ(cached_date_value(-2), std::string_view("Wed, 31 Dec 1969 23:59:58 GMT"));
    RUVIA_CHECK_EQ(cached_date_header(0), std::string_view("Date: Thu, 01 Jan 1970 00:00:00 GMT\r\n"));
    RUVIA_CHECK_EQ(cached_date_value(0), std::string_view("Thu, 01 Jan 1970 00:00:00 GMT"));
}

RUVIA_TEST(imf_fixdate_format_known_vector) {
    const auto date = std::get<0>(ruvia::detail::http_format_date(784111777));
    RUVIA_CHECK_EQ(date.size(), ruvia::http_imf_fixdate_size);
    RUVIA_CHECK_EQ(std::string(date.data(), date.size()), std::string("Sun, 06 Nov 1994 08:49:37 GMT"));
}

RUVIA_TEST(cached_date_header_framing_and_validity) {
    using ruvia::detail::cached_date_header;
    using ruvia::detail::cached_date_value;
    using ruvia::detail::http_parse_imf_fixdate;

    const std::string header(cached_date_header());
    RUVIA_CHECK_EQ(header.size(), std::size_t{37});  // "Date: " (6) + date (29) + CRLF (2)
    RUVIA_CHECK(header.starts_with("Date: "));
    RUVIA_CHECK(std::string_view(header).ends_with("\r\n"));

    // The 29-char value must parse as a valid IMF-fixdate; a localized or
    // malformed date (the pre-fix strftime %a/%b risk) would fail to parse.
    const std::string_view value_part = std::string_view(header).substr(6, 29);
    const auto parsed_from_header = http_parse_imf_fixdate(value_part);
    RUVIA_CHECK(parsed_from_header.has_value());
    if (parsed_from_header) {
        const auto now = std::time(nullptr);
        RUVIA_CHECK(*parsed_from_header <= now && now - *parsed_from_header < 3);
    }

    // The bare value accessor (HPACK :date) is likewise a valid 29-char date.
    const std::string value(cached_date_value());
    RUVIA_CHECK_EQ(value.size(), std::size_t{29});
    RUVIA_CHECK(http_parse_imf_fixdate(value).has_value());
}
