#include "field/field_value_scan.h"

#include <cstddef>
#include <string>
#include <string_view>

#include "test_harness.h"

RUVIA_TEST(http_field_value_prefix_locates_first_invalid_byte_at_each_word_offset) {
    const auto allowed = [](unsigned byte) {
        return byte == 9 || (byte >= 32 && byte != 127);
    };
    std::string value(24, 'a');
    for (std::size_t position = 0; position < 8; ++position) {
        for (unsigned first = 0; first < 256; ++first) {
            value[position] = static_cast<char>(first);
            for (unsigned second = 0; second < 256; ++second) {
                value[position + 1] = static_cast<char>(second);
                const auto expected = !allowed(first)    ? position
                                      : !allowed(second) ? position + 1
                                                         : value.size();
                RUVIA_CHECK_EQ(ruvia::detail::http_field_value_prefix_size(value), expected);
            }
        }
        value[position] = 'a';
        value[position + 1] = 'a';
    }
}

RUVIA_TEST(http_field_value_prefix_respects_borrowed_ranges_and_field_delimiters) {
    RUVIA_CHECK_EQ(ruvia::detail::http_field_value_prefix_size({}), std::size_t{0});
    for (std::size_t offset = 0; offset < 8; ++offset) {
        for (std::size_t length = 0; length < 96; ++length) {
            std::string storage(offset, '\x7f');
            for (std::size_t index = 0; index < length; ++index) {
                storage.push_back(index % 3 == 0 ? '\t' : index % 3 == 1 ? ' '
                                                                         : '\xe9');
            }
            storage.append("\r\nNext: value\r\n");
            storage.push_back('\0');
            const std::string_view value(storage.data() + offset, length);
            const std::string_view remaining(storage.data() + offset, storage.size() - offset);
            RUVIA_CHECK_EQ(ruvia::detail::http_field_value_prefix_size(value), length);
            RUVIA_CHECK_EQ(ruvia::detail::http_field_value_prefix_size(remaining), length);
            if (length != 0) {
                storage[offset + length - 1] = '\0';
                RUVIA_CHECK_EQ(ruvia::detail::http_field_value_prefix_size(value), length - 1);
            }
        }
    }
}
