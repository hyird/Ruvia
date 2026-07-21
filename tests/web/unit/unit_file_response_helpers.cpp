#include "test_harness.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <limits>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/detail/HttpDate.h"
#include "ruvia/http/detail/HttpEntityTag.h"
#include "ruvia/web/detail/StaticFileMetadata.h"

namespace {

using ruvia::detail::guessStaticFileContentType;

std::string_view guess(const char* name) {
    return guessStaticFileContentType(std::filesystem::path(name));
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

RUVIA_TEST(http_trim_weak_etag_prefix) {
    using ruvia::detail::httpTrimWeakEtagPrefix;
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("W/\"abc\""), std::string_view("\"abc\""));
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("\"abc\""), std::string_view("\"abc\""));  // strong etag unchanged
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("W/"), std::string_view(""));
    RUVIA_CHECK_EQ(httpTrimWeakEtagPrefix("W"), std::string_view("W"));  // needs both prefix chars
}

RUVIA_TEST(http_extension_equals_is_case_insensitive) {
    using ruvia::detail::staticFileExtensionEquals;
    RUVIA_CHECK(staticFileExtensionEquals(std::string_view("html"), "html"));
    RUVIA_CHECK(staticFileExtensionEquals(std::string_view("HTML"), "html"));
    RUVIA_CHECK(staticFileExtensionEquals(std::string_view("Json"), "json"));
    RUVIA_CHECK(staticFileExtensionEquals(std::string_view(""), ""));
    RUVIA_CHECK(!staticFileExtensionEquals(std::string_view("htm"), "html"));
    RUVIA_CHECK(!staticFileExtensionEquals(std::string_view("jpeg"), "json"));
}

RUVIA_TEST(http_append_unsigned_decimal) {
    using ruvia::detail::appendStaticFileUnsigned;
    std::pmr::string output(std::pmr::get_default_resource());
    appendStaticFileUnsigned(output, 0);
    RUVIA_CHECK_EQ(std::string_view(output), std::string_view("0"));
    output.clear();
    appendStaticFileUnsigned(output, 12345);
    RUVIA_CHECK_EQ(std::string_view(output), std::string_view("12345"));
    // Appends onto existing content rather than replacing it.
    appendStaticFileUnsigned(output, 67);
    RUVIA_CHECK_EQ(std::string_view(output), std::string_view("1234567"));
    // The 64-bit maximum.
    output.clear();
    appendStaticFileUnsigned(output, (std::numeric_limits<std::uint64_t>::max)());
    RUVIA_CHECK_EQ(std::string_view(output), std::string_view("18446744073709551615"));
}
