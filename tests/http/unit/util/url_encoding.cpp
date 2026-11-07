#include "ruvia/http/url_encoding.h"

#include <gtest/gtest.h>

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

std::optional<std::string> url_decode(std::string_view in, ruvia::url_decode_mode mode) {
    std::pmr::string storage;
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

TEST(Ruvia, url_decode_borrows_unchanged_input_and_decodes_into_storage) {
    std::pmr::monotonic_buffer_resource resource;
    std::pmr::string storage("keep", &resource);
    constexpr std::string_view plain = "value";
    const auto borrowed = ruvia::decode_url_component(plain, storage);
    EXPECT_TRUE(borrowed.has_value() && borrowed->data() == plain.data() && borrowed->size() == plain.size());
    EXPECT_EQ(std::string_view(storage), std::string_view("keep"));

    // Malformed percent encoding leaves the storage untouched.
    EXPECT_TRUE(!ruvia::decode_url_component("bad%2", storage).has_value());
    EXPECT_EQ(std::string_view(storage), std::string_view("keep"));

    const auto decoded = ruvia::decode_url_component("value%20one", storage);
    EXPECT_TRUE(decoded.has_value() && decoded->data() == storage.data());
    EXPECT_EQ(std::string_view(storage), std::string_view("value one"));
    EXPECT_EQ(storage.get_allocator().resource(), static_cast<std::pmr::memory_resource*>(&resource));

    // The input may be the storage itself.
    storage.assign("a%2Bb+c");
    const auto aliased = ruvia::decode_url_component(storage, storage, ruvia::url_decode_mode::form);
    EXPECT_TRUE(aliased.has_value() && aliased->data() == storage.data());
    EXPECT_EQ(std::string_view(storage), std::string_view("a+b c"));
}

TEST(Ruvia, uri_reference_encoding_borrows_or_writes_storage) {
    std::pmr::string storage("keep");
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
