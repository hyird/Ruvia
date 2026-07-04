#include "test_harness.h"

#include <memory_resource>
#include <optional>
#include <string_view>

#include "ruvia/http/UrlEncoding.h"

namespace {

using ruvia::detail::decodeUrlComponentToString;
using ruvia::detail::hasUrlEncoding;
using Mode = ruvia::detail::UrlDecodeMode;

}  // namespace

RUVIA_TEST(has_url_encoding_detection) {
    RUVIA_CHECK(hasUrlEncoding("a%20b", Mode::kPercent));
    RUVIA_CHECK(!hasUrlEncoding("abc", Mode::kPercent));
    // '+' is a literal in percent mode but triggers decoding in form mode.
    RUVIA_CHECK(!hasUrlEncoding("a+b", Mode::kPercent));
    RUVIA_CHECK(hasUrlEncoding("a+b", Mode::kForm));
    RUVIA_CHECK(hasUrlEncoding("a%20b", Mode::kForm));
    RUVIA_CHECK(!hasUrlEncoding("plain", Mode::kForm));
    RUVIA_CHECK(!hasUrlEncoding("", Mode::kForm));
}

RUVIA_TEST(no_encoding_means_decode_is_identity) {
    // The decode-skip fast path relies on: if hasUrlEncoding is false, decoding
    // reproduces the input verbatim.
    auto* const resource = std::pmr::get_default_resource();
    for (const std::string_view value : {std::string_view(""), std::string_view("plain"),
                                         std::string_view("a/b?c=d"), std::string_view("no-plus")}) {
        RUVIA_CHECK(!hasUrlEncoding(value, Mode::kPercent));
        const auto decoded = decodeUrlComponentToString(value, resource, Mode::kPercent);
        RUVIA_CHECK(decoded.has_value());
        RUVIA_CHECK_EQ(std::string_view(*decoded), value);
    }
}
