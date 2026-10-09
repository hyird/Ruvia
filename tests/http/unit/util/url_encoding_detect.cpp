#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/url_encoding.h"

#include "test_harness.h"

namespace {

using ruvia::decode_url_component;
using ruvia::has_url_encoding;
using mode_type = ruvia::url_decode_mode;

}  // namespace

RUVIA_TEST(has_url_encoding_detection) {
    RUVIA_CHECK(has_url_encoding("a%20b", mode_type::percent));
    RUVIA_CHECK(!has_url_encoding("abc", mode_type::percent));
    // '+' is a literal in percent mode but triggers decoding in form mode.
    RUVIA_CHECK(!has_url_encoding("a+b", mode_type::percent));
    RUVIA_CHECK(has_url_encoding("a+b", mode_type::form));
    RUVIA_CHECK(has_url_encoding("a%20b", mode_type::form));
    RUVIA_CHECK(!has_url_encoding("plain", mode_type::form));
    RUVIA_CHECK(!has_url_encoding("", mode_type::form));
    RUVIA_CHECK(!has_url_encoding("", mode_type::percent));
}

RUVIA_TEST(has_url_encoding_handles_binary_components) {
    std::string value(129, 'a');
    for (std::size_t position = 0; position < value.size(); ++position) {
        for (unsigned int byte = 0; byte < 256; ++byte) {
            value[position] = static_cast<char>(byte);
            RUVIA_CHECK_EQ(has_url_encoding(value, mode_type::percent), byte == '%');
            RUVIA_CHECK_EQ(has_url_encoding(value, mode_type::form), byte == '%' || byte == '+');
        }
        value[position] = 'a';
    }
}

RUVIA_TEST(has_url_encoding_respects_component_bounds) {
    const std::string storage = "%+" + std::string(129, 'a') + "+%";
    const auto value = std::string_view(storage).substr(2, 129);
    for (const auto mode : {mode_type::percent, mode_type::form}) {
        RUVIA_CHECK(!has_url_encoding(value, mode));
        RUVIA_CHECK(!has_url_encoding(value.substr(0, 0), mode));
        RUVIA_CHECK(!has_url_encoding(value.substr(value.size()), mode));
    }
    const std::string_view binary("a\0b%00c", 7);
    RUVIA_CHECK(has_url_encoding(binary, mode_type::percent));
    RUVIA_CHECK(has_url_encoding(binary, mode_type::form));
}

RUVIA_TEST(no_encoding_means_decode_is_identity) {
    // The decode-skip fast path relies on: if has_url_encoding is false, decoding
    // reproduces the input verbatim.
    auto* const resource = std::pmr::get_default_resource();
    for (const std::string_view value : {std::string_view(""), std::string_view("plain"),
             std::string_view("a/b?c=d"), std::string_view("no-plus")}) {
        RUVIA_CHECK(!has_url_encoding(value, mode_type::percent));
        const auto decoded =
            decode_url_component(value, {.mode_ = mode_type::percent, .resource_ = resource});
        RUVIA_CHECK(decoded.has_value());
        RUVIA_CHECK_EQ(std::string_view(*decoded), value);
    }
}
