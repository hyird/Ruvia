#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/UrlEncoding.h"

#include "test_harness.h"

namespace {

using ruvia::decodeUrlComponent;
using ruvia::hasUrlEncoding;
using Mode = ruvia::UrlDecodeMode;

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
    RUVIA_CHECK(!hasUrlEncoding("", Mode::kPercent));
}

RUVIA_TEST(has_url_encoding_handles_binary_components) {
    std::string value(129, 'a');
    for (std::size_t position = 0; position < value.size(); ++position) {
        for (unsigned int byte = 0; byte < 256; ++byte) {
            value[position] = static_cast<char>(byte);
            RUVIA_CHECK_EQ(hasUrlEncoding(value, Mode::kPercent), byte == '%');
            RUVIA_CHECK_EQ(hasUrlEncoding(value, Mode::kForm), byte == '%' || byte == '+');
        }
        value[position] = 'a';
    }
}

RUVIA_TEST(has_url_encoding_respects_component_bounds) {
    const std::string storage = "%+" + std::string(129, 'a') + "+%";
    const auto value = std::string_view(storage).substr(2, 129);
    for (const auto mode : {Mode::kPercent, Mode::kForm}) {
        RUVIA_CHECK(!hasUrlEncoding(value, mode));
        RUVIA_CHECK(!hasUrlEncoding(value.substr(0, 0), mode));
        RUVIA_CHECK(!hasUrlEncoding(value.substr(value.size()), mode));
    }
    const std::string_view binary("a\0b%00c", 7);
    RUVIA_CHECK(hasUrlEncoding(binary, Mode::kPercent));
    RUVIA_CHECK(hasUrlEncoding(binary, Mode::kForm));
}

RUVIA_TEST(no_encoding_means_decode_is_identity) {
    // The decode-skip fast path relies on: if hasUrlEncoding is false, decoding
    // reproduces the input verbatim.
    auto* const resource = std::pmr::get_default_resource();
    for (const std::string_view value : {std::string_view(""), std::string_view("plain"),
             std::string_view("a/b?c=d"), std::string_view("no-plus")}) {
        RUVIA_CHECK(!hasUrlEncoding(value, Mode::kPercent));
        const auto decoded =
            decodeUrlComponent(value, {.mode = Mode::kPercent, .resource = resource});
        RUVIA_CHECK(decoded.has_value());
        RUVIA_CHECK_EQ(std::string_view(*decoded), value);
    }
}
