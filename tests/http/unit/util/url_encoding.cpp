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
        RUVIA_CHECK(!url_decode(encoded + "%", mode).has_value());
        RUVIA_CHECK(!url_decode(encoded + "%0", mode).has_value());
        RUVIA_CHECK(!url_decode(encoded + "%xz", mode).has_value());
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

RUVIA_TEST(url_component_equals) {
    using m_type = ruvia::url_decode_mode;
    using ruvia::url_component_equals;
    RUVIA_CHECK(url_component_equals("%41bc", "Abc", m_type::percent));
    RUVIA_CHECK(url_component_equals("a+b", "a b", m_type::form));
    RUVIA_CHECK(!url_component_equals("a+b", "a b", m_type::percent));  // '+' literal
    RUVIA_CHECK(!url_component_equals("abc", "abcd", m_type::percent));
    RUVIA_CHECK(!url_component_equals("abcd", "abc", m_type::percent));
    RUVIA_CHECK(!url_component_equals("%2", "x", m_type::percent));  // truncated escape
}

RUVIA_TEST(url_find_pair_value) {
    using m_type = ruvia::url_decode_mode;
    using ruvia::find_url_encoded_value;
    const std::string_view q = "a=1&b=two&flag&c=%41";
    RUVIA_CHECK_EQ(find_url_encoded_value(q, "a", m_type::percent).value_or("?"), std::string_view("1"));
    RUVIA_CHECK_EQ(find_url_encoded_value(q, "b", m_type::percent).value_or("?"), std::string_view("two"));
    RUVIA_CHECK_EQ(find_url_encoded_value(q, "c", m_type::percent).value_or("?"), std::string_view("%41"));
    // key present with no '=' yields empty value, not missing
    RUVIA_CHECK(find_url_encoded_value(q, "flag", m_type::percent).has_value());
    RUVIA_CHECK_EQ(find_url_encoded_value(q, "flag", m_type::percent).value(), std::string_view(""));
    RUVIA_CHECK(!find_url_encoded_value(q, "missing", m_type::percent).has_value());
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

RUVIA_TEST(url_find_pair_value_uses_last_duplicate) {
    const auto value = ruvia::find_url_encoded_value("item=first&%69tem=last", "item",
        ruvia::url_decode_mode::percent);
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(*value, std::string_view("last"));
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
