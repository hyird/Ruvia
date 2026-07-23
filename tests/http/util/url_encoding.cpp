#include "test_harness.h"

#include <cstdint>
#include <limits>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "ruvia/core/detail/util/Base64.h"
#include "ruvia/core/detail/util/Base64Url.h"
#include "ruvia/http/detail/util/HttpNumberFormat.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/http/detail/util/Hex.h"

namespace {

std::optional<std::string> urlDecode(std::string_view in, ruvia::detail::UrlDecodeMode mode) {
    auto decoded = ruvia::detail::decodeUrlComponent(
        in,
        mode,
        std::pmr::get_default_resource());
    if (!decoded.has_value()) {
        return std::nullopt;
    }
    return std::string(*decoded);
}

}  // namespace

// --- Base64 (RFC 4648 test vectors) --------------------------------------

// --- Hex nibble ----------------------------------------------------------

// --- URL decoding --------------------------------------------------------

// --- Number formatting ---------------------------------------------------

// Percent-encoding: decoding a component or a form field, validating one, and walking encoded pairs.

RUVIA_TEST(url_decode_percent) {
    using M = ruvia::detail::UrlDecodeMode;
    RUVIA_CHECK_EQ(urlDecode("hello", M::kPercent).value(), std::string("hello"));
    RUVIA_CHECK_EQ(urlDecode("%41%42%43", M::kPercent).value(), std::string("ABC"));
    RUVIA_CHECK_EQ(urlDecode("a%2Fb", M::kPercent).value(), std::string("a/b"));
    RUVIA_CHECK_EQ(urlDecode("%00", M::kPercent).value(), std::string(1, '\0'));
    // '+' is literal in percent mode
    RUVIA_CHECK_EQ(urlDecode("a+b", M::kPercent).value(), std::string("a+b"));
}

RUVIA_TEST(url_decode_form) {
    using M = ruvia::detail::UrlDecodeMode;
    RUVIA_CHECK_EQ(urlDecode("a+b", M::kForm).value(), std::string("a b"));
    RUVIA_CHECK_EQ(urlDecode("a+b%20c", M::kForm).value(), std::string("a b c"));
}

RUVIA_TEST(url_decode_invalid) {
    using M = ruvia::detail::UrlDecodeMode;
    RUVIA_CHECK(!urlDecode("%", M::kPercent).has_value());
    RUVIA_CHECK(!urlDecode("%4", M::kPercent).has_value());
    RUVIA_CHECK(!urlDecode("%zz", M::kPercent).has_value());
    RUVIA_CHECK(!urlDecode("ab%2", M::kPercent).has_value());
    RUVIA_CHECK(!urlDecode("%g0", M::kPercent).has_value());
}

RUVIA_TEST(url_validate_encoding) {
    using ruvia::detail::validateUrlEncoding;
    RUVIA_CHECK(validateUrlEncoding("plain"));
    RUVIA_CHECK(validateUrlEncoding("%41%42"));
    RUVIA_CHECK(!validateUrlEncoding("%4"));
    RUVIA_CHECK(!validateUrlEncoding("%zz"));
    RUVIA_CHECK(validateUrlEncoding(""));
}

RUVIA_TEST(url_component_equals) {
    using M = ruvia::detail::UrlDecodeMode;
    using ruvia::detail::urlComponentEquals;
    RUVIA_CHECK(urlComponentEquals("%41bc", "Abc", M::kPercent));
    RUVIA_CHECK(urlComponentEquals("a+b", "a b", M::kForm));
    RUVIA_CHECK(!urlComponentEquals("a+b", "a b", M::kPercent));  // '+' literal
    RUVIA_CHECK(!urlComponentEquals("abc", "abcd", M::kPercent));
    RUVIA_CHECK(!urlComponentEquals("abcd", "abc", M::kPercent));
    RUVIA_CHECK(!urlComponentEquals("%2", "x", M::kPercent));  // truncated escape
}

RUVIA_TEST(url_find_pair_value) {
    using M = ruvia::detail::UrlDecodeMode;
    using ruvia::detail::findUrlEncodedValue;
    const std::string_view q = "a=1&b=two&flag&c=%41";
    RUVIA_CHECK_EQ(findUrlEncodedValue(q, "a", M::kPercent).value_or("?"), std::string_view("1"));
    RUVIA_CHECK_EQ(findUrlEncodedValue(q, "b", M::kPercent).value_or("?"), std::string_view("two"));
    RUVIA_CHECK_EQ(findUrlEncodedValue(q, "c", M::kPercent).value_or("?"), std::string_view("%41"));
    // key present with no '=' yields empty value, not missing
    RUVIA_CHECK(findUrlEncodedValue(q, "flag", M::kPercent).has_value());
    RUVIA_CHECK_EQ(findUrlEncodedValue(q, "flag", M::kPercent).value(), std::string_view(""));
    RUVIA_CHECK(!findUrlEncodedValue(q, "missing", M::kPercent).has_value());
}

RUVIA_TEST(url_visit_pairs_count) {
    std::vector<std::pair<std::string, std::string>> pairs;
    (void)ruvia::detail::visitUrlEncodedPairs(
        "x=1&y=2&z=3", [&](std::string_view n, std::string_view v) {
            pairs.emplace_back(std::string(n), std::string(v));
        });
    RUVIA_CHECK_EQ(pairs.size(), std::size_t(3));
    if (pairs.size() == 3) {
        RUVIA_CHECK_EQ(pairs[0].first, std::string("x"));
        RUVIA_CHECK_EQ(pairs[2].second, std::string("3"));
    }
}

RUVIA_TEST(url_visit_pairs_skips_empty_segments) {
    std::vector<std::pair<std::string, std::string>> pairs;
    // Leading, doubled, and trailing '&' produce empty segments that must NOT yield ("","") pairs.
    (void)ruvia::detail::visitUrlEncodedPairs(
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
    (void)ruvia::detail::visitUrlEncodedPairs(
        "k=&=v", [&](std::string_view n, std::string_view v) {
            kept.emplace_back(std::string(n), std::string(v));
        });
    RUVIA_CHECK_EQ(kept.size(), std::size_t(2));  // "k=" (name k, empty value) and "=v" (empty name, value v)
}
