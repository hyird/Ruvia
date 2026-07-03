#include "test_harness.h"

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "http/FileResponseHelpers.h"

namespace {

using ruvia::detail::httpGuessContentType;
using ruvia::detail::httpParseByteRange;

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
    RUVIA_CHECK_EQ(guess("icon.svg"), std::string_view("image/svg+xml"));
    RUVIA_CHECK_EQ(guess("mod.wasm"), std::string_view("application/wasm"));
    // Unknown extension and no extension fall back to a non-executable octet-stream.
    RUVIA_CHECK_EQ(guess("archive.xyz"), std::string_view("application/octet-stream"));
    RUVIA_CHECK_EQ(guess("noext"), std::string_view("application/octet-stream"));
}

RUVIA_TEST(byte_range_bounded_and_open_ended) {
    // bytes=first-last within a 1000-byte resource.
    const auto bounded = httpParseByteRange("bytes=100-199", 1000);
    RUVIA_CHECK(bounded.has_value());
    RUVIA_CHECK_EQ(bounded->offset, std::uint64_t{100});
    RUVIA_CHECK_EQ(bounded->length, std::uint64_t{100});
    // Open-ended: from an offset to the end.
    const auto open = httpParseByteRange("bytes=500-", 1000);
    RUVIA_CHECK(open.has_value());
    RUVIA_CHECK_EQ(open->offset, std::uint64_t{500});
    RUVIA_CHECK_EQ(open->length, std::uint64_t{500});
    // Last byte only.
    const auto lastByte = httpParseByteRange("bytes=999-999", 1000);
    RUVIA_CHECK(lastByte.has_value());
    RUVIA_CHECK_EQ(lastByte->offset, std::uint64_t{999});
    RUVIA_CHECK_EQ(lastByte->length, std::uint64_t{1});
    // An end past the resource is clamped to the last byte.
    const auto clampedEnd = httpParseByteRange("bytes=0-2000", 1000);
    RUVIA_CHECK(clampedEnd.has_value());
    RUVIA_CHECK_EQ(clampedEnd->offset, std::uint64_t{0});
    RUVIA_CHECK_EQ(clampedEnd->length, std::uint64_t{1000});
}

RUVIA_TEST(byte_range_suffix) {
    // bytes=-N is the last N bytes.
    const auto suffix = httpParseByteRange("bytes=-100", 1000);
    RUVIA_CHECK(suffix.has_value());
    RUVIA_CHECK_EQ(suffix->offset, std::uint64_t{900});
    RUVIA_CHECK_EQ(suffix->length, std::uint64_t{100});
    // A suffix larger than the resource clamps to the whole resource.
    const auto whole = httpParseByteRange("bytes=-2000", 1000);
    RUVIA_CHECK(whole.has_value());
    RUVIA_CHECK_EQ(whole->offset, std::uint64_t{0});
    RUVIA_CHECK_EQ(whole->length, std::uint64_t{1000});
}

RUVIA_TEST(byte_range_rejects_invalid) {
    RUVIA_CHECK(!httpParseByteRange("bytes=1000-", 1000).has_value());        // start at/after end
    RUVIA_CHECK(!httpParseByteRange("bytes=500-100", 1000).has_value());      // last < first
    RUVIA_CHECK(!httpParseByteRange("bytes=-0", 1000).has_value());           // zero-length suffix
    RUVIA_CHECK(!httpParseByteRange("bytes=0-99,200-299", 1000).has_value()); // multiple ranges
    RUVIA_CHECK(!httpParseByteRange("0-99", 1000).has_value());               // missing "bytes=" unit
    RUVIA_CHECK(!httpParseByteRange("bytes=abc", 1000).has_value());          // no '-'
    RUVIA_CHECK(!httpParseByteRange("bytes=", 1000).has_value());             // empty spec
    RUVIA_CHECK(!httpParseByteRange("bytes=0-99", 0).has_value());            // empty resource
}
