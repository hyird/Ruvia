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

#include "Base64.h"
#include "detail/HttpNumberFormat.h"
#include "ruvia/http/UrlEncoding.h"
#include "ruvia/http/detail/Hex.h"

namespace {

std::string b64(std::string_view in) {
    std::string out(ruvia::detail::base64EncodedSize(in.size()), '\0');
    ruvia::detail::encodeBase64(
        out.data(),
        std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(in.data()), in.size()));
    return out;
}

std::optional<std::string> urlDecode(std::string_view in, ruvia::detail::UrlDecodeMode mode) {
    std::string out;
    if (!ruvia::detail::decodeUrlComponent(in, out, mode)) {
        return std::nullopt;
    }
    return out;
}

}  // namespace

// --- Base64 (RFC 4648 test vectors) --------------------------------------
RUVIA_TEST(base64_rfc4648_vectors) {
    RUVIA_CHECK_EQ(b64(""), std::string(""));
    RUVIA_CHECK_EQ(b64("f"), std::string("Zg=="));
    RUVIA_CHECK_EQ(b64("fo"), std::string("Zm8="));
    RUVIA_CHECK_EQ(b64("foo"), std::string("Zm9v"));
    RUVIA_CHECK_EQ(b64("foob"), std::string("Zm9vYg=="));
    RUVIA_CHECK_EQ(b64("fooba"), std::string("Zm9vYmE="));
    RUVIA_CHECK_EQ(b64("foobar"), std::string("Zm9vYmFy"));
}

RUVIA_TEST(base64_encoded_size) {
    using ruvia::detail::base64EncodedSize;
    RUVIA_CHECK_EQ(base64EncodedSize(0), std::size_t(0));
    RUVIA_CHECK_EQ(base64EncodedSize(1), std::size_t(4));
    RUVIA_CHECK_EQ(base64EncodedSize(2), std::size_t(4));
    RUVIA_CHECK_EQ(base64EncodedSize(3), std::size_t(4));
    RUVIA_CHECK_EQ(base64EncodedSize(4), std::size_t(8));
    RUVIA_CHECK_EQ(base64EncodedSize(32), std::size_t(44));  // HMAC-SHA256
}

RUVIA_TEST(base64_binary_high_bytes) {
    const unsigned char bytes[] = {0xFF, 0x00, 0xFF};
    std::string out(4, '\0');
    ruvia::detail::encodeBase64(out.data(),
                                std::span<const std::uint8_t>(bytes, sizeof(bytes)));
    RUVIA_CHECK_EQ(out, std::string("/wD/"));
}

// --- Hex nibble ----------------------------------------------------------
RUVIA_TEST(hex_nibble) {
    using ruvia::detail::decodeHexNibble;
    RUVIA_CHECK_EQ(decodeHexNibble('0'), 0);
    RUVIA_CHECK_EQ(decodeHexNibble('9'), 9);
    RUVIA_CHECK_EQ(decodeHexNibble('a'), 10);
    RUVIA_CHECK_EQ(decodeHexNibble('f'), 15);
    RUVIA_CHECK_EQ(decodeHexNibble('A'), 10);
    RUVIA_CHECK_EQ(decodeHexNibble('F'), 15);
    RUVIA_CHECK_EQ(decodeHexNibble('g'), -1);
    RUVIA_CHECK_EQ(decodeHexNibble('G'), -1);
    RUVIA_CHECK_EQ(decodeHexNibble('/'), -1);
    RUVIA_CHECK_EQ(decodeHexNibble(':'), -1);
}

// --- URL decoding --------------------------------------------------------
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

// --- Number formatting ---------------------------------------------------
RUVIA_TEST(number_unsigned_decimal_size) {
    using ruvia::detail::httpUnsignedDecimalSize;
    RUVIA_CHECK_EQ(httpUnsignedDecimalSize(0), std::size_t(1));
    RUVIA_CHECK_EQ(httpUnsignedDecimalSize(9), std::size_t(1));
    RUVIA_CHECK_EQ(httpUnsignedDecimalSize(10), std::size_t(2));
    RUVIA_CHECK_EQ(httpUnsignedDecimalSize(99), std::size_t(2));
    RUVIA_CHECK_EQ(httpUnsignedDecimalSize(100), std::size_t(3));
    RUVIA_CHECK_EQ(httpUnsignedDecimalSize(UINT64_C(18446744073709551615)), std::size_t(20));
}

RUVIA_TEST(number_append_formatted) {
    std::pmr::string out(std::pmr::get_default_resource());
    ruvia::detail::appendHttpFormattedNumber(out, 42, "err");
    ruvia::detail::appendHttpFormattedNumber(out, -7, "err");
    RUVIA_CHECK_EQ(std::string(out.c_str()), std::string("42-7"));
}

RUVIA_TEST(number_append_formatted_finite_rejects_non_finite) {
    // A finite double formats as usual.
    std::pmr::string out(std::pmr::get_default_resource());
    ruvia::detail::appendHttpFormattedFiniteNumber(out, 3.5, "not finite", "bad format");
    RUVIA_CHECK_EQ(std::string(out.c_str()), std::string("3.5"));

    // NaN and both infinities are rejected rather than emitted as the words
    // "nan"/"inf", which are not valid SQL/RESP numeric literals and would splice
    // in unquoted. Nothing is appended on rejection.
    for (const double bad : {std::numeric_limits<double>::quiet_NaN(),
                             std::numeric_limits<double>::infinity(),
                             -std::numeric_limits<double>::infinity()}) {
        std::pmr::string sink(std::pmr::get_default_resource());
        bool threw = false;
        try {
            ruvia::detail::appendHttpFormattedFiniteNumber(sink, bad, "not finite", "bad format");
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        RUVIA_CHECK(threw);
        RUVIA_CHECK(sink.empty());
    }
}
