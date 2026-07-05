#include "test_harness.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "http/FileResponseHelpers.h"

namespace {

using ruvia::detail::httpGuessContentType;
using ruvia::detail::HttpRangeOutcome;
using ruvia::detail::httpParseByteRange;
using ruvia::detail::httpParseUnsigned;

std::string_view guess(const char* name) {
    return httpGuessContentType(std::filesystem::path(name));
}

}  // namespace

RUVIA_TEST(content_type_guessing) {
    RUVIA_CHECK_EQ(guess("index.html"), std::string_view("text/html; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("page.htm"), std::string_view("text/html; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("style.css"), std::string_view("text/css; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("app.js"), std::string_view("text/javascript; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("mod.mjs"), std::string_view("text/javascript; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("data.json"), std::string_view("application/json; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("photo.png"), std::string_view("image/png"));
    RUVIA_CHECK_EQ(guess("photo.jpeg"), std::string_view("image/jpeg"));
    // ".jpg" is a distinct token in the same branch as ".jpeg" and is the far more
    // common spelling -- pin it so a regression can't silently serve it as a
    // download (octet-stream) instead of an image.
    RUVIA_CHECK_EQ(guess("photo.jpg"), std::string_view("image/jpeg"));
    RUVIA_CHECK_EQ(guess("anim.gif"), std::string_view("image/gif"));
    RUVIA_CHECK_EQ(guess("notes.txt"), std::string_view("text/plain; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("server.log"), std::string_view("text/plain; charset=utf-8"));
    RUVIA_CHECK_EQ(guess("icon.svg"), std::string_view("image/svg+xml"));
    RUVIA_CHECK_EQ(guess("mod.wasm"), std::string_view("application/wasm"));
    // The extension match is case-insensitive end to end, so an uppercase extension
    // maps just like its lowercase form (not to the octet-stream fallback).
    RUVIA_CHECK_EQ(guess("PHOTO.PNG"), std::string_view("image/png"));
    RUVIA_CHECK_EQ(guess("PAGE.HTML"), std::string_view("text/html; charset=utf-8"));
    // Unknown extension and no extension fall back to a non-executable octet-stream.
    RUVIA_CHECK_EQ(guess("archive.xyz"), std::string_view("application/octet-stream"));
    RUVIA_CHECK_EQ(guess("noext"), std::string_view("application/octet-stream"));
}

RUVIA_TEST(parse_unsigned_accepts_and_rejects) {
    RUVIA_CHECK_EQ(httpParseUnsigned("0").value(), std::uint64_t{0});
    RUVIA_CHECK_EQ(httpParseUnsigned("42").value(), std::uint64_t{42});
    RUVIA_CHECK_EQ(httpParseUnsigned("007").value(), std::uint64_t{7});  // leading zeros allowed
    RUVIA_CHECK_EQ(httpParseUnsigned("18446744073709551615").value(),
                   std::numeric_limits<std::uint64_t>::max());

    // Rejections return nullopt: empty, overflow (not wrapped), a sign, leading
    // whitespace, and trailing non-digits.
    RUVIA_CHECK(!httpParseUnsigned("").has_value());
    RUVIA_CHECK(!httpParseUnsigned("18446744073709551616").has_value());  // > UINT64_MAX
    RUVIA_CHECK(!httpParseUnsigned("+5").has_value());
    RUVIA_CHECK(!httpParseUnsigned("-5").has_value());
    RUVIA_CHECK(!httpParseUnsigned(" 5").has_value());
    RUVIA_CHECK(!httpParseUnsigned("5x").has_value());
}

RUVIA_TEST(byte_range_bounded_and_open_ended) {
    // bytes=first-last within a 1000-byte resource.
    const auto bounded = httpParseByteRange("bytes=100-199", 1000);
    RUVIA_CHECK(bounded.outcome == HttpRangeOutcome::kSatisfiable);
    RUVIA_CHECK_EQ(bounded.range.offset, std::uint64_t{100});
    RUVIA_CHECK_EQ(bounded.range.length, std::uint64_t{100});
    // Open-ended: from an offset to the end.
    const auto open = httpParseByteRange("bytes=500-", 1000);
    RUVIA_CHECK(open.outcome == HttpRangeOutcome::kSatisfiable);
    RUVIA_CHECK_EQ(open.range.offset, std::uint64_t{500});
    RUVIA_CHECK_EQ(open.range.length, std::uint64_t{500});
    // Last byte only.
    const auto lastByte = httpParseByteRange("bytes=999-999", 1000);
    RUVIA_CHECK(lastByte.outcome == HttpRangeOutcome::kSatisfiable);
    RUVIA_CHECK_EQ(lastByte.range.offset, std::uint64_t{999});
    RUVIA_CHECK_EQ(lastByte.range.length, std::uint64_t{1});
    // An end past the resource is clamped to the last byte.
    const auto clampedEnd = httpParseByteRange("bytes=0-2000", 1000);
    RUVIA_CHECK(clampedEnd.outcome == HttpRangeOutcome::kSatisfiable);
    RUVIA_CHECK_EQ(clampedEnd.range.offset, std::uint64_t{0});
    RUVIA_CHECK_EQ(clampedEnd.range.length, std::uint64_t{1000});
}

RUVIA_TEST(byte_range_suffix) {
    // bytes=-N is the last N bytes.
    const auto suffix = httpParseByteRange("bytes=-100", 1000);
    RUVIA_CHECK(suffix.outcome == HttpRangeOutcome::kSatisfiable);
    RUVIA_CHECK_EQ(suffix.range.offset, std::uint64_t{900});
    RUVIA_CHECK_EQ(suffix.range.length, std::uint64_t{100});
    // A suffix larger than the resource clamps to the whole resource.
    const auto whole = httpParseByteRange("bytes=-2000", 1000);
    RUVIA_CHECK(whole.outcome == HttpRangeOutcome::kSatisfiable);
    RUVIA_CHECK_EQ(whole.range.offset, std::uint64_t{0});
    RUVIA_CHECK_EQ(whole.range.length, std::uint64_t{1000});
}

RUVIA_TEST(byte_range_unsatisfiable_yields_416) {
    // Well-formed ranges that fall outside the representation -> 416 (RFC 9110
    // §15.5.17): a Content-Range: bytes */size must be sent, so these are distinct
    // from an ignored range.
    RUVIA_CHECK(httpParseByteRange("bytes=1000-", 1000).outcome ==
                HttpRangeOutcome::kUnsatisfiable);  // start at/after end
    RUVIA_CHECK(httpParseByteRange("bytes=1500-1600", 1000).outcome ==
                HttpRangeOutcome::kUnsatisfiable);  // whole range past end
    RUVIA_CHECK(httpParseByteRange("bytes=-0", 1000).outcome ==
                HttpRangeOutcome::kUnsatisfiable);  // zero-length suffix
    RUVIA_CHECK(httpParseByteRange("bytes=0-99", 0).outcome ==
                HttpRangeOutcome::kUnsatisfiable);  // any range against an empty resource
}

RUVIA_TEST(byte_range_malformed_or_unknown_unit_is_ignored) {
    // RFC 9110 §14.2: an unknown range unit MUST be ignored (served as a full 200),
    // and a syntactically invalid byte range is likewise ignored rather than 416'd
    // -- otherwise a client's typo or unsupported unit would deny it the resource.
    RUVIA_CHECK(httpParseByteRange("items=0-99", 1000).outcome ==
                HttpRangeOutcome::kIgnore);  // unknown range unit (MUST ignore)
    RUVIA_CHECK(httpParseByteRange("0-99", 1000).outcome ==
                HttpRangeOutcome::kIgnore);  // missing "bytes=" unit
    RUVIA_CHECK(httpParseByteRange("bytes=500-100", 1000).outcome ==
                HttpRangeOutcome::kIgnore);  // last < first (invalid range-spec)
    RUVIA_CHECK(httpParseByteRange("bytes=abc", 1000).outcome ==
                HttpRangeOutcome::kIgnore);  // no '-'
    RUVIA_CHECK(httpParseByteRange("bytes=x-9", 1000).outcome ==
                HttpRangeOutcome::kIgnore);  // non-digit first-pos
    RUVIA_CHECK(httpParseByteRange("bytes=0-x", 1000).outcome ==
                HttpRangeOutcome::kIgnore);  // non-digit last-pos
    RUVIA_CHECK(httpParseByteRange("bytes=-x", 1000).outcome ==
                HttpRangeOutcome::kIgnore);  // non-digit suffix-length
    RUVIA_CHECK(httpParseByteRange("bytes=-", 1000).outcome ==
                HttpRangeOutcome::kIgnore);  // empty suffix-length
    RUVIA_CHECK(httpParseByteRange("bytes=", 1000).outcome ==
                HttpRangeOutcome::kIgnore);  // empty spec
    RUVIA_CHECK(httpParseByteRange("bytes=0-99,200-299", 1000).outcome ==
                HttpRangeOutcome::kIgnore);  // multiple ranges -> served as full 200
}

RUVIA_TEST(byte_range_set_multiple_detection) {
    using ruvia::detail::httpByteRangeSetHasMultiple;
    // A comma after "bytes=" means more than one range was requested.
    RUVIA_CHECK(httpByteRangeSetHasMultiple("bytes=0-99,200-299"));
    RUVIA_CHECK(httpByteRangeSetHasMultiple("bytes=0-0,-1"));
    // A single range, a suffix, and non-range / prefixless inputs are not "multiple".
    RUVIA_CHECK(!httpByteRangeSetHasMultiple("bytes=0-99"));
    RUVIA_CHECK(!httpByteRangeSetHasMultiple("bytes=-100"));
    RUVIA_CHECK(!httpByteRangeSetHasMultiple("bytes="));
    RUVIA_CHECK(!httpByteRangeSetHasMultiple("0-99,200-299"));  // missing "bytes=" unit
}

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

    // The two-digit RFC 850 year uses the POSIX pivot: 68 => 2068 (future),
    // 69 => 1969 (before the Unix epoch).
    RUVIA_CHECK(httpParseHttpDate("Wed, 01-Jan-68 00:00:00 GMT").value_or(-1) > canonical);
    RUVIA_CHECK(httpParseHttpDate("Wed, 01-Jan-69 00:00:00 GMT").value_or(0) < std::time_t{0});

    // A malformed instance of each obsolete format is rejected, not silently
    // coerced through the shared assembler.
    RUVIA_CHECK(!httpParseHttpDate("Sunday, 06-Xxx-94 08:49:37 GMT").has_value());  // bad month (RFC 850)
    RUVIA_CHECK(!httpParseHttpDate("Sunday, 06-Nov-94 08:49:37 UTC").has_value());  // not GMT (RFC 850)
    RUVIA_CHECK(!httpParseHttpDate("Sun Xxx  6 08:49:37 1994").has_value());        // bad month (asctime)
    RUVIA_CHECK(!httpParseHttpDate("garbage").has_value());
}

RUVIA_TEST(http_trim_weak_etag_prefix) {
    using ruvia::detail::httpTrimWeakEtagPrefix;
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("W/\"abc\""), std::string_view("\"abc\""));
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("\"abc\""), std::string_view("\"abc\""));  // strong etag unchanged
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("W/"), std::string_view(""));
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("W"), std::string_view("W"));  // needs both prefix chars
}

RUVIA_TEST(http_extension_equals_is_case_insensitive) {
    using ruvia::detail::httpExtensionEquals;
    RUVIA_CHECK(httpExtensionEquals(std::string_view("html"), "html"));
    RUVIA_CHECK(httpExtensionEquals(std::string_view("HTML"), "html"));  // extension case-folded
    RUVIA_CHECK(httpExtensionEquals(std::string_view("Json"), "json"));
    RUVIA_CHECK(httpExtensionEquals(std::string_view(""), ""));
    RUVIA_CHECK(!httpExtensionEquals(std::string_view("htm"), "html"));   // length differs
    RUVIA_CHECK(!httpExtensionEquals(std::string_view("jpeg"), "json"));  // content differs
}

RUVIA_TEST(http_append_unsigned_decimal) {
    using ruvia::detail::httpAppendUnsigned;
    std::pmr::string output(std::pmr::get_default_resource());
    httpAppendUnsigned(output, 0);
    RUVIA_CHECK_EQ(std::string_view(output), std::string_view("0"));
    output.clear();
    httpAppendUnsigned(output, 12345);
    RUVIA_CHECK_EQ(std::string_view(output), std::string_view("12345"));
    // Appends onto existing content rather than replacing it.
    httpAppendUnsigned(output, 67);
    RUVIA_CHECK_EQ(std::string_view(output), std::string_view("1234567"));
    // The 64-bit maximum.
    output.clear();
    httpAppendUnsigned(output, (std::numeric_limits<std::uint64_t>::max)());
    RUVIA_CHECK_EQ(std::string_view(output), std::string_view("18446744073709551615"));
}
