#include "test_harness.h"

#include <cstddef>
#include <ctime>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>

#include "http/FileResponseHelpers.h"
#include "http/HttpImfFixdate.h"

namespace {

std::string formatDate(std::time_t time) {
    const auto out = ruvia::detail::httpFormatDate(std::pmr::get_default_resource(), time);
    return std::string(out.data(), out.size());
}

std::string fileEtag(std::uint64_t size, std::filesystem::file_time_type::duration ticks) {
    const auto out = ruvia::detail::httpMakeFileEtag(
        std::pmr::get_default_resource(), size, std::filesystem::file_time_type{ticks});
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
    RUVIA_CHECK(!httpParseImfFixdate("Thu; 01 Jan 1970 00:00:00 GMT").has_value());   // wrong separator
}

RUVIA_TEST(imf_fixdate_leap_second_boundary) {
    using ruvia::detail::httpParseImfFixdate;
    RUVIA_CHECK(httpParseImfFixdate("Thu, 01 Jan 1970 23:59:60 GMT").has_value());   // leap second allowed
    RUVIA_CHECK(!httpParseImfFixdate("Thu, 01 Jan 1970 23:59:61 GMT").has_value());  // 61 rejected
}

// httpFormatDate must emit RFC 7231 IMF-fixdate with English day/month names
// independent of the process locale (regression: it used strftime %a/%b).
RUVIA_TEST(http_format_date_known_vectors) {
    RUVIA_CHECK_EQ(formatDate(0), std::string("Thu, 01 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(formatDate(86400), std::string("Fri, 02 Jan 1970 00:00:00 GMT"));
    RUVIA_CHECK_EQ(formatDate(784111777), std::string("Sun, 06 Nov 1994 08:49:37 GMT"));
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
    using Ticks = std::filesystem::file_time_type::duration;
    const auto base = fileEtag(100, Ticks{123456});
    RUVIA_CHECK_EQ(base, fileEtag(100, Ticks{123456}));       // deterministic
    RUVIA_CHECK(base != fileEtag(101, Ticks{123456}));        // size-sensitive
    RUVIA_CHECK(base != fileEtag(100, Ticks{123457}));        // mtime-sensitive
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
