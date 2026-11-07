#include <gtest/gtest.h>

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/http/url_encoding.h"

namespace {

using ruvia::decode_url_component;
using ruvia::has_url_encoding;
using mode_type = ruvia::url_decode_mode;

}  // namespace

TEST(Ruvia, has_url_encoding_detection) {
    EXPECT_TRUE(has_url_encoding("a%20b", mode_type::percent));
    EXPECT_TRUE(!has_url_encoding("abc", mode_type::percent));
    // '+' is a literal in percent mode but triggers decoding in form mode.
    EXPECT_TRUE(!has_url_encoding("a+b", mode_type::percent));
    EXPECT_TRUE(has_url_encoding("a+b", mode_type::form));
    EXPECT_TRUE(has_url_encoding("a%20b", mode_type::form));
    EXPECT_TRUE(!has_url_encoding("plain", mode_type::form));
    EXPECT_TRUE(!has_url_encoding("", mode_type::form));
    EXPECT_TRUE(!has_url_encoding("", mode_type::percent));
}

TEST(Ruvia, has_url_encoding_handles_binary_components) {
    std::string value(129, 'a');
    for (std::size_t position = 0; position < value.size(); ++position) {
        for (unsigned int byte = 0; byte < 256; ++byte) {
            value[position] = static_cast<char>(byte);
            EXPECT_EQ(has_url_encoding(value, mode_type::percent), byte == '%');
            EXPECT_EQ(has_url_encoding(value, mode_type::form), byte == '%' || byte == '+');
        }
        value[position] = 'a';
    }
}

TEST(Ruvia, has_url_encoding_respects_component_bounds) {
    const std::string storage = "%+" + std::string(129, 'a') + "+%";
    const auto value = std::string_view(storage).substr(2, 129);
    for (const auto mode : {mode_type::percent, mode_type::form}) {
        EXPECT_TRUE(!has_url_encoding(value, mode));
        EXPECT_TRUE(!has_url_encoding(value.substr(0, 0), mode));
        EXPECT_TRUE(!has_url_encoding(value.substr(value.size()), mode));
    }
    const std::string_view binary("a\0b%00c", 7);
    EXPECT_TRUE(has_url_encoding(binary, mode_type::percent));
    EXPECT_TRUE(has_url_encoding(binary, mode_type::form));
}

TEST(Ruvia, no_encoding_means_decode_borrows_the_input) {
    // Callers size their decoded-storage reservations with has_url_encoding:
    // a component it reports unencoded is returned as the input itself.
    std::pmr::string storage;
    for (const std::string_view value : {std::string_view(""), std::string_view("plain"),
             std::string_view("a/b?c=d"), std::string_view("no-plus")}) {
        for (const auto mode : {mode_type::percent, mode_type::form}) {
            EXPECT_TRUE(!has_url_encoding(value, mode));
            const auto decoded = decode_url_component(value, storage, mode);
            EXPECT_TRUE(decoded.has_value() && decoded->data() == value.data() && decoded->size() == value.size());
        }
    }
    EXPECT_TRUE(storage.empty());
}
