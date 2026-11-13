#include "ruvia/http/url_encoding.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/http/http_field_syntax.h"

namespace {
using ruvia::decode_url_component;
using ruvia::has_url_encoding;
using mode_type = ruvia::url_decode_mode;

std::optional<std::string> url_decode(std::string_view in, ruvia::url_decode_mode mode) {
    std::string storage;
    const auto decoded = ruvia::decode_url_component(in, storage, mode);
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    return std::string(*decoded);
}

}  // namespace

// Percent-encoding: decoding a component or a form field, validating one, and walking encoded
// pairs.

TEST(Ruvia, url_decode_percent) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_EQ(url_decode("hello", m_type::percent).value(), std::string("hello"));
    EXPECT_EQ(url_decode("%41%42%43", m_type::percent).value(), std::string("ABC"));
    EXPECT_EQ(url_decode("a%2Fb", m_type::percent).value(), std::string("a/b"));
    EXPECT_EQ(url_decode("%00", m_type::percent).value(), std::string(1, '\0'));
    // '+' is literal in percent mode
    EXPECT_EQ(url_decode("a+b", m_type::percent).value(), std::string("a+b"));
}

TEST(Ruvia, url_decode_form) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_EQ(url_decode("a+b", m_type::form).value(), std::string("a b"));
    EXPECT_EQ(url_decode("a+b%20c", m_type::form).value(), std::string("a b c"));
}

TEST(Ruvia, percent_byte_decoding_rejects_out_of_range_and_truncated_triplets) {
    EXPECT_TRUE(ruvia::http_try_decode_percent_byte("a%2Fb", 1) == std::optional<unsigned char>('/'));
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("abc", std::string_view::npos).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("abc", 3).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("%2", 0).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("%G0", 0).has_value());
    EXPECT_TRUE(!ruvia::http_try_decode_percent_byte("x20", 0).has_value());
}

TEST(Ruvia, uri_reference_encoding_borrows_or_writes_storage) {
    std::string storage("keep");
    constexpr std::string_view canonical = "/a%20b?x=1#frag";
    const auto unchanged = ruvia::percent_encode_uri_reference(canonical, storage);
    EXPECT_TRUE(unchanged.data() == canonical.data() && unchanged.size() == canonical.size());
    EXPECT_EQ(std::string_view(storage), std::string_view("keep"));

    const auto encoded = ruvia::percent_encode_uri_reference("/a b?x=%zz", storage);
    EXPECT_TRUE(encoded.data() == storage.data());
    EXPECT_EQ(encoded, std::string_view("/a%20b?x=%25zz"));

    // The reference may be the storage itself.
    storage.assign("/c d");
    EXPECT_EQ(ruvia::percent_encode_uri_reference(storage, storage), std::string_view("/c%20d"));
    EXPECT_EQ(std::string_view(storage), std::string_view("/c%20d"));
}

TEST(Ruvia, url_decode_invalid) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_TRUE(!url_decode("%", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("%4", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("%zz", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("ab%2", m_type::percent).has_value());
    EXPECT_TRUE(!url_decode("%g0", m_type::percent).has_value());
}

TEST(Ruvia, url_decode_form_keeps_lone_percent_literally) {
    using m_type = ruvia::url_decode_mode;
    EXPECT_EQ(url_decode("100%", m_type::form).value_or("<none>"), std::string("100%"));
    EXPECT_EQ(url_decode("%zz+%41", m_type::form).value_or("<none>"), std::string("%zz A"));
    EXPECT_EQ(url_decode("%4%41", m_type::form).value_or("<none>"), std::string("%4A"));
}

TEST(Ruvia, url_decode_all_bytes_and_malformed_suffix) {
    using m_type = ruvia::url_decode_mode;
    constexpr std::string_view hex = "0123456789aBcDeF";
    std::string encoded;
    std::string expected;
    for (unsigned int byte = 0; byte < 256; ++byte) {
        encoded.push_back('%');
        encoded.push_back(hex[byte >> 4]);
        encoded.push_back(hex[byte & 15]);
        expected.push_back(static_cast<char>(byte));
    }
    for (const auto mode : {m_type::percent, m_type::form}) {
        const auto result_value = url_decode(encoded, mode);
        EXPECT_TRUE(result_value.has_value());
        if (result_value) {
            EXPECT_EQ(*result_value, expected);
        }
        for (const std::string_view suffix : {"%", "%0", "%xz"}) {
            const auto suffixed = url_decode(encoded + std::string(suffix), mode);
            if (mode == m_type::percent) {
                EXPECT_TRUE(!suffixed.has_value());
            } else {
                // WHATWG percent-decode keeps a '%' that starts no valid escape.
                EXPECT_TRUE(suffixed.has_value() && *suffixed == expected + std::string(suffix));
            }
        }
    }
}

TEST(Ruvia, url_validate_encoding) {
    using ruvia::validate_url_encoding;
    EXPECT_TRUE(validate_url_encoding("plain"));
    EXPECT_TRUE(validate_url_encoding("%41%42"));
    EXPECT_TRUE(!validate_url_encoding("%4"));
    EXPECT_TRUE(!validate_url_encoding("%zz"));
    EXPECT_TRUE(validate_url_encoding(""));
}

TEST(Ruvia, url_uri_paths_equivalent_and_ordered_by_canonical_form) {
    using ruvia::compare_uri_paths;
    using ruvia::uri_paths_equivalent;
    EXPECT_TRUE(uri_paths_equivalent("/users/admin", "/users/%61dmin"));
    EXPECT_TRUE(uri_paths_equivalent("/caf%C3%A9", "/caf%c3%a9"));
    EXPECT_TRUE(uri_paths_equivalent("/a:b", "/a%3Ab"));  // decoded, like route parameters
    EXPECT_TRUE(uri_paths_equivalent("", ""));
    EXPECT_TRUE(!uri_paths_equivalent("/a/b", "/a%2Fb"));  // an encoded slash is no separator
    EXPECT_TRUE(uri_paths_equivalent("/a%2fb", "/a%2Fb"));
    EXPECT_TRUE(!uri_paths_equivalent("/a+b", "/a%20b"));  // '+' is literal in a path
    EXPECT_TRUE(!uri_paths_equivalent("/api", "/apix"));
    EXPECT_TRUE(!uri_paths_equivalent("/%zz", "/%zz"));  // malformed escape
    EXPECT_TRUE(!uri_paths_equivalent("/%2", "/%2"));    // truncated escape

    EXPECT_EQ(compare_uri_paths("%61", "a"), 0);
    EXPECT_TRUE(compare_uri_paths("a", "%62") < 0);
    EXPECT_TRUE(compare_uri_paths("%62", "a") > 0);
    EXPECT_TRUE(compare_uri_paths("ab", "%61") > 0);
    EXPECT_TRUE(compare_uri_paths("a", "a%2F") < 0);
    // A separator orders after every octet, an encoded slash among them.
    EXPECT_TRUE(compare_uri_paths("a/", "a%FF") > 0);
    EXPECT_TRUE(compare_uri_paths("a%2F", "a/") < 0);
}

TEST(Ruvia, url_visit_pairs_count) {
    std::vector<std::pair<std::string, std::string>> pairs;
    (void)ruvia::visit_url_encoded_pairs(
        "x=1&y=2&z=3", [&](std::string_view n, std::string_view v) {
            pairs.emplace_back(std::string(n), std::string(v));
        });
    EXPECT_EQ(pairs.size(), std::size_t(3));
    if (pairs.size() == 3) {
        EXPECT_EQ(pairs[0].first, std::string("x"));
        EXPECT_EQ(pairs[2].second, std::string("3"));
    }
}

TEST(Ruvia, url_visit_pairs_stops_early) {
    std::size_t visited = 0;
    const bool completed = ruvia::visit_url_encoded_pairs("a=1&b=2&c=3",
        [&visited](std::string_view, std::string_view) {
            return ++visited < 2;
        });
    EXPECT_TRUE(!completed);
    EXPECT_EQ(visited, std::size_t(2));
}

TEST(Ruvia, url_visit_pairs_skips_empty_segments) {
    std::vector<std::pair<std::string, std::string>> pairs;
    // Leading, doubled, and trailing '&' produce empty segments that must NOT yield ("","") pairs.
    (void)ruvia::visit_url_encoded_pairs(
        "&a=1&&b=2&", [&](std::string_view n, std::string_view v) {
            pairs.emplace_back(std::string(n), std::string(v));
        });
    EXPECT_EQ(pairs.size(), std::size_t(2));
    if (pairs.size() == 2) {
        EXPECT_EQ(pairs[0].first, std::string("a"));
        EXPECT_EQ(pairs[1].first, std::string("b"));
    }
    // A key with an empty value ("k=") is still a real field and must be kept.
    std::vector<std::pair<std::string, std::string>> kept;
    (void)ruvia::visit_url_encoded_pairs("k=&=v", [&](std::string_view n, std::string_view v) {
        kept.emplace_back(std::string(n), std::string(v));
    });
    EXPECT_EQ(kept.size(), std::size_t(2));  // "k=" (name k, empty value) and "=v" (empty name, value v)
}

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
    std::string storage;
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

TEST(Ruvia, hex_nibbles_decode_and_encode_ascii_digits) {
    EXPECT_EQ(ruvia::http_decode_hex_nibble('0'), 0);
    EXPECT_EQ(ruvia::http_decode_hex_nibble('a'), 10);
    EXPECT_EQ(ruvia::http_decode_hex_nibble('F'), 15);
    EXPECT_EQ(ruvia::http_decode_hex_nibble('g'), -1);
    EXPECT_EQ(ruvia::http_lower_hex_digit(15), 'f');
    EXPECT_EQ(ruvia::http_upper_hex_digit(15), 'F');
}

TEST(Ruvia, hex_encode_accepts_output_overlapping_input) {
    constexpr std::string_view text =
        "ab\x01\xff"
        "cdef";
    constexpr std::size_t input_start = 16;
    for (std::size_t size = 0; size <= text.size(); ++size) {
        const auto input = text.substr(0, size);
        std::string expected(2 * size, '\0');
        ruvia::http_encode_lower_hex(expected.data(), input);
        // Every output start from wholly before to wholly after the input,
        // including in place and starts inside it.
        for (auto output_start = input_start - expected.size(); output_start <= input_start + size; ++output_start) {
            std::array<char, 48> buffer{};
            input.copy(buffer.data() + input_start, size);
            const auto* const end =
                ruvia::http_encode_lower_hex(buffer.data() + output_start, std::string_view(buffer.data() + input_start, size));
            EXPECT_EQ(end, buffer.data() + output_start + expected.size());
            EXPECT_EQ(std::string(buffer.data() + output_start, expected.size()), expected);
        }
    }
}

TEST(Ruvia, hex_append_accepts_input_aliasing_the_output) {
    std::string whole("\x01\xab");
    whole.shrink_to_fit();
    ruvia::http_append_lower_hex(whole, std::string_view(whole));
    EXPECT_EQ(std::string_view(whole), std::string_view("\x01\xab"
                                                        "01ab"));

    // A long prefix forces reallocation while the source is a strict subrange.
    std::string part(std::string(64, 'z') + "\xff");
    part.shrink_to_fit();
    const std::span<const unsigned char> tail(reinterpret_cast<const unsigned char*>(part.data()) + 63, 2);
    ruvia::http_append_lower_hex(part, tail);
    EXPECT_EQ(std::string_view(part).substr(65), std::string_view("7aff"));

    // The terminating NUL is readable input: it ends where the digits begin.
    std::string terminated("A");
    terminated.shrink_to_fit();
    ruvia::http_append_lower_hex(terminated, std::string_view(terminated.data(), terminated.size() + 1));
    EXPECT_EQ(std::string_view(terminated), std::string_view("A4100"));

    // A full string reallocates while the NUL-only input borrows the old buffer.
    std::string nul_only("A");
    nul_only.resize(nul_only.capacity(), 'B');
    const auto filled = nul_only.size();
    ruvia::http_append_lower_hex(nul_only, std::string_view(nul_only.data() + filled, 1));
    EXPECT_EQ(nul_only.size(), filled + 2);
    EXPECT_EQ(std::string_view(nul_only).substr(filled - 1), std::string_view("B00"));
}

namespace {

std::string encode(std::string_view input, ruvia::http_base64_padding padding = ruvia::http_base64_padding::required) {
    std::string output(ruvia::http_base64_encoded_size(input.size(), padding), '\0');
    ruvia::http_base64_encode(output.data(),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(input.data()), input.size()),
        ruvia::http_base64_charset::standard, padding);
    return output;
}

}  // namespace

TEST(Ruvia, http_base64_encodes_with_standard_alphabet_and_padding) {
    EXPECT_EQ(encode(""), std::string(""));
    EXPECT_EQ(encode("f"), std::string("Zg=="));
    EXPECT_EQ(encode("fo"), std::string("Zm8="));
    EXPECT_EQ(encode("foo"), std::string("Zm9v"));
    EXPECT_EQ(encode("foobar"), std::string("Zm9vYmFy"));
    EXPECT_EQ(encode("\xfb\xff"), std::string("+/8="));
}

TEST(Ruvia, http_base64_encode_accepts_output_overlapping_input) {
    constexpr std::string_view text = "abcdefghij";
    constexpr std::size_t input_start = 16;
    for (const auto padding : {ruvia::http_base64_padding::required, ruvia::http_base64_padding::forbidden}) {
        for (std::size_t size = 0; size <= text.size(); ++size) {
            const auto input = text.substr(0, size);
            const auto expected = encode(input, padding);
            // Every output start from wholly before to wholly after the input,
            // including in place and starts inside it.
            for (auto output_start = input_start - expected.size(); output_start <= input_start + size; ++output_start) {
                std::array<char, 48> buffer{};
                input.copy(buffer.data() + input_start, size);
                ruvia::http_base64_encode(buffer.data() + output_start,
                    std::span(reinterpret_cast<const std::uint8_t*>(buffer.data()) + input_start, size),
                    ruvia::http_base64_charset::standard, padding);
                EXPECT_EQ(std::string(buffer.data() + output_start, expected.size()), expected);
            }
        }
    }
}

TEST(Ruvia, http_base64_encoded_size_preserves_padding_at_numeric_boundaries) {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    constexpr auto maximum_groups = maximum / 4;
    constexpr auto maximum_input = maximum_groups * 3;
    constexpr auto maximum_output = maximum_groups * 4;
    constexpr auto small_output = ruvia::http_base64_encoded_size(4);
    EXPECT_EQ(small_output, std::size_t{8});
    for (const auto input_size : {maximum_input - 2, maximum_input - 1, maximum_input}) {
        EXPECT_EQ(ruvia::http_base64_encoded_size(input_size), maximum_output);
    }
    for (const auto input_size : {maximum_input + 1, maximum - 1, maximum}) {
        bool rejected = false;
        try {
            (void)ruvia::http_base64_encoded_size(input_size);
        } catch (const std::length_error&) {
            rejected = true;
        }
        EXPECT_TRUE(rejected);
    }
}

TEST(Ruvia, http_base64_unpadded_size_uses_actual_tail_at_numeric_boundary) {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    constexpr auto groups = maximum / 4;
    constexpr auto forbidden = ruvia::http_base64_padding::forbidden;
    EXPECT_EQ(ruvia::http_base64_encoded_size(3 * groups + 1, forbidden), maximum - 1);
    EXPECT_EQ(ruvia::http_base64_encoded_size(3 * groups + 2, forbidden), maximum);
    bool rejected = false;
    try {
        (void)ruvia::http_base64_encoded_size(3 * groups + 3, forbidden);
    } catch (const std::length_error&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
}

TEST(Ruvia, http_base64_decodes_single_characters_per_alphabet) {
    for (std::size_t i = 0; i < ruvia::http_base64_alphabet.size(); ++i) {
        EXPECT_EQ(ruvia::http_base64_decode_char(ruvia::http_base64_alphabet[i]), static_cast<int>(i));
        EXPECT_EQ(ruvia::http_base64_url_decode_char(ruvia::http_base64_url_alphabet[i]), static_cast<int>(i));
    }
    EXPECT_EQ(ruvia::http_base64_decode_char('='), -1);
    EXPECT_EQ(ruvia::http_base64_decode_char('-'), -1);
    EXPECT_EQ(ruvia::http_base64_url_decode_char('+'), -1);
    EXPECT_EQ(ruvia::http_base64_url_decode_char('/'), -1);
    EXPECT_EQ(ruvia::http_base64_url_decode_char('='), -1);
}

TEST(Ruvia, http_base64_decode_rejects_output_overwriting_unread_input) {
    constexpr auto standard = ruvia::http_base64_charset::standard;
    constexpr auto required = ruvia::http_base64_padding::required;
    const auto decoded_text = [](const std::uint8_t* bytes, std::size_t size) {
        return std::string(reinterpret_cast<const char*>(bytes), size);
    };

    std::uint8_t shifted[16] = {'Q', 'U', 'J', 'D', 'R', 'E', 'V', 'G'};
    const std::string_view shifted_input(reinterpret_cast<const char*>(shifted), 8);
    EXPECT_TRUE(!ruvia::http_base64_decode(shifted_input, std::span(shifted + 4, 6), standard, required));
    EXPECT_EQ(decoded_text(shifted, 8), std::string("QUJDREVG"));

    std::uint8_t in_place[8] = {'Q', 'U', 'J', 'D', 'R', 'E', 'V', 'G'};
    const std::string_view in_place_input(reinterpret_cast<const char*>(in_place), 8);
    const auto in_place_size = ruvia::http_base64_decode(in_place_input, std::span(in_place), standard, required);
    EXPECT_TRUE(in_place_size && *in_place_size == 6);
    EXPECT_EQ(decoded_text(in_place, 6), std::string("ABCDEF"));

    std::uint8_t after[16] = {'Q', 'U', 'J', 'D', 'R', 'E', 'V', 'G'};
    const std::string_view after_input(reinterpret_cast<const char*>(after), 8);
    const auto after_size = ruvia::http_base64_decode(after_input, std::span(after + 8, 8), standard, required);
    EXPECT_TRUE(after_size && *after_size == 6);
    EXPECT_EQ(decoded_text(after + 8, 6), std::string("ABCDEF"));
}

namespace {

using ruvia::http_ascii_equals_ignore_case;

}  // namespace

TEST(Ruvia, http_ascii_equals_ignore_case_folds_only_letters) {
    EXPECT_TRUE(http_ascii_equals_ignore_case("Text/HTML", "text/html"));
    EXPECT_TRUE(http_ascii_equals_ignore_case("", ""));
    EXPECT_TRUE(!http_ascii_equals_ignore_case("abc", "abcd"));
    EXPECT_TRUE(!http_ascii_equals_ignore_case("abc", "abd"));

    EXPECT_TRUE(!http_ascii_equals_ignore_case("[", "{"));
    EXPECT_TRUE(!http_ascii_equals_ignore_case("@", "`"));

    EXPECT_TRUE(http_ascii_equals_ignore_case(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\xA9", 2)));
    EXPECT_TRUE(!http_ascii_equals_ignore_case(
        std::string_view("\xC3\xA9", 2), std::string_view("\xC3\x89", 2)));
}

TEST(Ruvia, number_format_decimal_width_and_string_output) {
    EXPECT_EQ(ruvia::http_unsigned_decimal_size(0), std::size_t{1});
    EXPECT_EQ(ruvia::http_unsigned_decimal_size(999), std::size_t{3});
    EXPECT_EQ(ruvia::http_unsigned_decimal_size((std::numeric_limits<std::uint64_t>::max)()), std::size_t{20});

    std::string output;
    ruvia::http_append_formatted_number(output, std::int64_t{-42}, "format failure");
    ruvia::http_append_formatted_number(output, std::uint64_t{123}, "format failure");
    EXPECT_EQ(output, "-42123");
    ruvia::http_append_formatted_finite_number(output, 1.5, "nonfinite", "format failure");
    EXPECT_EQ(output, "-421231.5");
}

TEST(Ruvia, number_format_rejects_nonfinite_without_modifying_output) {
    std::string output("prefix");
    bool rejected = false;
    try {
        ruvia::http_append_formatted_finite_number(output, std::numeric_limits<double>::infinity(),
            "nonfinite", "format failure");
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(output, "prefix");
}

TEST(Ruvia, number_format_signed_extremes) {
    std::string output;
    ruvia::http_append_signed(output, (std::numeric_limits<std::int64_t>::min)());
    EXPECT_EQ(output, "-9223372036854775808");
    output.clear();
    ruvia::http_append_signed(output, (std::numeric_limits<std::int64_t>::max)());
    EXPECT_EQ(output, "9223372036854775807");
}
