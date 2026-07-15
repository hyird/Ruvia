#include "test_harness.h"

#include <cstddef>
#include <ctime>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/http/detail/HttpDate.h"
#include "ruvia/http/detail/HttpEntityTag.h"
#include "ruvia/http/detail/HttpImfFixdate.h"
#include "ruvia/http/detail/server/HttpDateCache.h"
#include "ruvia/web/detail/StaticFileMetadata.h"

namespace {

std::string formatDate(std::time_t time) {
    const auto out = ruvia::detail::httpFormatDate(std::pmr::get_default_resource(), time);
    return std::string(out.data(), out.size());
}

std::string fileEtag(
    std::uint64_t size,
    std::uint64_t modifiedToken,
    ruvia::detail::ResponseFileIdentity identity) {
    const auto out = ruvia::detail::makeStaticFileSnapshotEtag(
        std::pmr::get_default_resource(), size, modifiedToken, identity);
    return std::string(out.data(), out.size());
}

}  // namespace

// ETag comparison and IMF-fixdate parsing back the conditional-request handling
// (If-Match / If-None-Match / If-Range, RFC 7232) for static file responses.

RUVIA_TEST(etag_strong_comparison) {
    using ruvia::detail::httpStrongEtagEquals;
    // Strong compare: both must be strong (no "W/") and octet-equal.
    RUVIA_CHECK(httpStrongEtagEquals(R"("abc")", R"("abc")"));
    RUVIA_CHECK(!httpStrongEtagEquals(R"("abc")", R"("abd")"));       // different tag
    RUVIA_CHECK(!httpStrongEtagEquals(R"(W/"abc")", R"("abc")"));     // one weak -> never strong-equal
    RUVIA_CHECK(!httpStrongEtagEquals(R"(W/"abc")", R"(W/"abc")"));   // both weak -> not a strong match
}

RUVIA_TEST(etag_weak_comparison) {
    using ruvia::detail::httpWeakEtagEquals;
    // Weak compare: equal after stripping an optional "W/" prefix.
    RUVIA_CHECK(httpWeakEtagEquals(R"(W/"abc")", R"("abc")"));
    RUVIA_CHECK(httpWeakEtagEquals(R"(W/"abc")", R"(W/"abc")"));
    RUVIA_CHECK(httpWeakEtagEquals(R"("abc")", R"("abc")"));
    RUVIA_CHECK(!httpWeakEtagEquals(R"(W/"abc")", R"(W/"abd")"));     // different opaque tags
}

RUVIA_TEST(etag_weak_prefix_detection) {
    using ruvia::detail::httpIsWeakEtag;
    RUVIA_CHECK(httpIsWeakEtag(R"(W/"abc")"));
    RUVIA_CHECK(!httpIsWeakEtag(R"("abc")"));
    RUVIA_CHECK(!httpIsWeakEtag("W"));  // too short to be the "W/" marker
}

RUVIA_TEST(etag_list_parses_opaque_commas_and_rejects_malformed_suffixes) {
    using ruvia::detail::httpEtagListMatches;
    using ruvia::detail::httpParseEtagListMatches;
    RUVIA_CHECK(httpEtagListMatches(R"("stale,tag", "current")", R"("current")", true));
    RUVIA_CHECK(httpEtagListMatches(R"("stale", W/"current")", R"("current")", false));
    RUVIA_CHECK(!httpEtagListMatches(R"("stale, "current")", R"("current")", true));
    RUVIA_CHECK(!httpEtagListMatches(R"("current" trailing)", R"("current")", false));
    const auto malformedAfterMatch = httpParseEtagListMatches(
        R"("current", malformed)", R"("current")", true);
    RUVIA_CHECK(!malformedAfterMatch.valid);
    RUVIA_CHECK(!malformedAfterMatch.matched);
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

// httpFormatDate must emit RFC 7231 IMF-fixdate with English day/month names
// independent of the process locale (regression: it used strftime %a/%b).
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

RUVIA_TEST(file_etag_deterministic_and_sensitive) {
    const auto identity = ruvia::detail::ResponseFileIdentity::checked(
        {1, 2, 3, 4});
    const auto replacement = ruvia::detail::ResponseFileIdentity::checked(
        {1, 2, 3, 5});
    const auto base = fileEtag(100, 123456, identity);
    // The strong validator binds framing metadata and the exact indexed file.
    RUVIA_CHECK_EQ(base, std::string("\"100-123456-1-2-3-4\""));
    RUVIA_CHECK_EQ(base, fileEtag(100, 123456, identity));
    RUVIA_CHECK(base != fileEtag(101, 123456, identity));
    RUVIA_CHECK(base != fileEtag(100, 123457, identity));
    RUVIA_CHECK(base != fileEtag(100, 123456, replacement));
    RUVIA_CHECK(base.size() >= 2 && base.front() == '"' && base.back() == '"');  // quoted-string
}

// httpWriteImfFixdate is the single owner of HTTP date formatting (used by both
// httpFormatDate/Last-Modified and the response Date header cache). Test it in
// isolation with a hand-built tm so no gmtime dependency is involved.
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

// The response Date header cache must emit "Date: <IMF-fixdate>\r\n" with a valid
// English date reflecting the current second (guards the shared formatter on the
// hot per-response path).
RUVIA_TEST(cached_date_header_framing_and_validity) {
    using ruvia::detail::cachedDateHeader;
    using ruvia::detail::cachedDateValue;
    using ruvia::detail::httpParseImfFixdate;

    const std::string header(cachedDateHeader());
    RUVIA_CHECK_EQ(header.size(), std::size_t{37});  // "Date: " (6) + date (29) + CRLF (2)
    RUVIA_CHECK(header.starts_with("Date: "));
    RUVIA_CHECK(std::string_view(header).substr(header.size() - 2) == "\r\n");

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
