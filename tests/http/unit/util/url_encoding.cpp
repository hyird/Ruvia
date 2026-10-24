#include "ruvia/http/url_encoding.h"

#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "test_harness.h"

namespace {

std::optional<std::string> url_decode(std::string_view in, ruvia::url_decode_mode mode) {
    auto decoded = ruvia::decode_url_component(
        in, {.mode_ = mode, .resource_ = std::pmr::get_default_resource()});
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    return std::string(*decoded);
}

}  // namespace

// Percent-encoding: decoding a component or a form field, validating one, and walking encoded
// pairs.

RUVIA_TEST(url_decode_percent) {
    using m_type = ruvia::url_decode_mode;
    RUVIA_CHECK_EQ(url_decode("hello", m_type::percent).value(), std::string("hello"));
    RUVIA_CHECK_EQ(url_decode("%41%42%43", m_type::percent).value(), std::string("ABC"));
    RUVIA_CHECK_EQ(url_decode("a%2Fb", m_type::percent).value(), std::string("a/b"));
    RUVIA_CHECK_EQ(url_decode("%00", m_type::percent).value(), std::string(1, '\0'));
    // '+' is literal in percent mode
    RUVIA_CHECK_EQ(url_decode("a+b", m_type::percent).value(), std::string("a+b"));
}

RUVIA_TEST(url_decode_form) {
    using m_type = ruvia::url_decode_mode;
    RUVIA_CHECK_EQ(url_decode("a+b", m_type::form).value(), std::string("a b"));
    RUVIA_CHECK_EQ(url_decode("a+b%20c", m_type::form).value(), std::string("a b c"));
}

RUVIA_TEST(percent_byte_decoding_rejects_out_of_range_and_truncated_triplets) {
    RUVIA_CHECK(ruvia::http_try_decode_percent_byte("a%2Fb", 1) == std::optional<unsigned char>('/'));
    RUVIA_CHECK(!ruvia::http_try_decode_percent_byte("abc", std::string_view::npos).has_value());
    RUVIA_CHECK(!ruvia::http_try_decode_percent_byte("abc", 3).has_value());
    RUVIA_CHECK(!ruvia::http_try_decode_percent_byte("%2", 0).has_value());
    RUVIA_CHECK(!ruvia::http_try_decode_percent_byte("%G0", 0).has_value());
    RUVIA_CHECK(!ruvia::http_try_decode_percent_byte("x20", 0).has_value());
}

RUVIA_TEST(url_decode_uses_requested_resource_and_preserves_results) {
    std::pmr::monotonic_buffer_resource resource;
    auto first = ruvia::decode_url_component("value%20one", {.resource_ = &resource});
    auto second = ruvia::decode_url_component("value%20two", {.resource_ = &resource});
    RUVIA_CHECK(first.has_value());
    RUVIA_CHECK(second.has_value());
    if (first && second) {
        RUVIA_CHECK_EQ(first->get_allocator().resource(), &resource);
        RUVIA_CHECK_EQ(std::string_view(*first), std::string_view("value one"));
        RUVIA_CHECK_EQ(std::string_view(*second), std::string_view("value two"));
    }
}

RUVIA_TEST(url_decode_invalid) {
    using m_type = ruvia::url_decode_mode;
    RUVIA_CHECK(!url_decode("%", m_type::percent).has_value());
    RUVIA_CHECK(!url_decode("%4", m_type::percent).has_value());
    RUVIA_CHECK(!url_decode("%zz", m_type::percent).has_value());
    RUVIA_CHECK(!url_decode("ab%2", m_type::percent).has_value());
    RUVIA_CHECK(!url_decode("%g0", m_type::percent).has_value());
}

RUVIA_TEST(url_decode_form_keeps_lone_percent_literally) {
    using m_type = ruvia::url_decode_mode;
    RUVIA_CHECK_EQ(url_decode("100%", m_type::form).value_or("<none>"), std::string("100%"));
    RUVIA_CHECK_EQ(url_decode("%zz+%41", m_type::form).value_or("<none>"), std::string("%zz A"));
    RUVIA_CHECK_EQ(url_decode("%4%41", m_type::form).value_or("<none>"), std::string("%4A"));
}

RUVIA_TEST(url_decode_all_bytes_and_malformed_suffix) {
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
        RUVIA_CHECK(result_value.has_value());
        if (result_value) {
            RUVIA_CHECK_EQ(*result_value, expected);
        }
        for (const std::string_view suffix : {"%", "%0", "%xz"}) {
            const auto suffixed = url_decode(encoded + std::string(suffix), mode);
            if (mode == m_type::percent) {
                RUVIA_CHECK(!suffixed.has_value());
            } else {
                // WHATWG percent-decode keeps a '%' that starts no valid escape.
                RUVIA_CHECK(suffixed.has_value() && *suffixed == expected + std::string(suffix));
            }
        }
    }
}

RUVIA_TEST(url_validate_encoding) {
    using ruvia::validate_url_encoding;
    RUVIA_CHECK(validate_url_encoding("plain"));
    RUVIA_CHECK(validate_url_encoding("%41%42"));
    RUVIA_CHECK(!validate_url_encoding("%4"));
    RUVIA_CHECK(!validate_url_encoding("%zz"));
    RUVIA_CHECK(validate_url_encoding(""));
}

RUVIA_TEST(url_components_equivalent) {
    using m_type = ruvia::url_decode_mode;
    using ruvia::url_components_equivalent;
    RUVIA_CHECK(url_components_equivalent("%61pi", "api", m_type::percent));
    RUVIA_CHECK(url_components_equivalent("api", "%61%70%69", m_type::percent));
    RUVIA_CHECK(url_components_equivalent("%2f", "%2F", m_type::percent));
    RUVIA_CHECK(url_components_equivalent("", "", m_type::percent));
    RUVIA_CHECK(url_components_equivalent("a+b", "a%20b", m_type::form));
    RUVIA_CHECK(!url_components_equivalent("a+b", "a%20b", m_type::percent));  // '+' literal
    RUVIA_CHECK(!url_components_equivalent("%61pi", "apix", m_type::percent));
    RUVIA_CHECK(!url_components_equivalent("apix", "%61pi", m_type::percent));
    RUVIA_CHECK(!url_components_equivalent("%2", "%2", m_type::percent));    // truncated escape
    RUVIA_CHECK(!url_components_equivalent("%zz", "%zz", m_type::percent));  // malformed escape
}

RUVIA_TEST(url_uri_paths_equivalent_and_ordered_by_canonical_form) {
    using ruvia::compare_uri_paths;
    using ruvia::uri_paths_equivalent;
    RUVIA_CHECK(uri_paths_equivalent("/users/admin", "/users/%61dmin"));
    RUVIA_CHECK(uri_paths_equivalent("/caf%C3%A9", "/caf%c3%a9"));
    RUVIA_CHECK(uri_paths_equivalent("/a:b", "/a%3Ab"));  // decoded, like route parameters
    RUVIA_CHECK(uri_paths_equivalent("", ""));
    RUVIA_CHECK(!uri_paths_equivalent("/a/b", "/a%2Fb"));  // an encoded slash is no separator
    RUVIA_CHECK(uri_paths_equivalent("/a%2fb", "/a%2Fb"));
    RUVIA_CHECK(!uri_paths_equivalent("/a+b", "/a%20b"));  // '+' is literal in a path
    RUVIA_CHECK(!uri_paths_equivalent("/api", "/apix"));
    RUVIA_CHECK(!uri_paths_equivalent("/%zz", "/%zz"));  // malformed escape
    RUVIA_CHECK(!uri_paths_equivalent("/%2", "/%2"));    // truncated escape

    RUVIA_CHECK_EQ(compare_uri_paths("%61", "a"), 0);
    RUVIA_CHECK(compare_uri_paths("a", "%62") < 0);
    RUVIA_CHECK(compare_uri_paths("%62", "a") > 0);
    RUVIA_CHECK(compare_uri_paths("ab", "%61") > 0);
    RUVIA_CHECK(compare_uri_paths("a", "a%2F") < 0);
    // A separator orders after every octet, an encoded slash among them.
    RUVIA_CHECK(compare_uri_paths("a/", "a%FF") > 0);
    RUVIA_CHECK(compare_uri_paths("a%2F", "a/") < 0);
}

RUVIA_TEST(url_visit_pairs_count) {
    std::vector<std::pair<std::string, std::string>> pairs;
    (void)ruvia::visit_url_encoded_pairs(
        "x=1&y=2&z=3", [&](std::string_view n, std::string_view v) {
            pairs.emplace_back(std::string(n), std::string(v));
        });
    RUVIA_CHECK_EQ(pairs.size(), std::size_t(3));
    if (pairs.size() == 3) {
        RUVIA_CHECK_EQ(pairs[0].first, std::string("x"));
        RUVIA_CHECK_EQ(pairs[2].second, std::string("3"));
    }
}

RUVIA_TEST(url_visit_pairs_stops_early) {
    std::size_t visited = 0;
    const bool completed = ruvia::visit_url_encoded_pairs("a=1&b=2&c=3",
        [&visited](std::string_view, std::string_view) {
            return ++visited < 2;
        });
    RUVIA_CHECK(!completed);
    RUVIA_CHECK_EQ(visited, std::size_t(2));
}

RUVIA_TEST(url_visit_pairs_skips_empty_segments) {
    std::vector<std::pair<std::string, std::string>> pairs;
    // Leading, doubled, and trailing '&' produce empty segments that must NOT yield ("","") pairs.
    (void)ruvia::visit_url_encoded_pairs(
        "&a=1&&b=2&", [&](std::string_view n, std::string_view v) {
            pairs.emplace_back(std::string(n), std::string(v));
        });
    RUVIA_CHECK_EQ(pairs.size(), std::size_t(2));
    if (pairs.size() == 2) {
        RUVIA_CHECK_EQ(pairs[0].first, std::string("a"));
        RUVIA_CHECK_EQ(pairs[1].first, std::string("b"));
    }
    // A key with an empty value ("k=") is still a real field and must be kept.
    std::vector<std::pair<std::string, std::string>> kept;
    (void)ruvia::visit_url_encoded_pairs("k=&=v", [&](std::string_view n, std::string_view v) {
        kept.emplace_back(std::string(n), std::string(v));
    });
    RUVIA_CHECK_EQ(
        kept.size(), std::size_t(2));  // "k=" (name k, empty value) and "=v" (empty name, value v)
}
