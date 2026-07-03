#include "test_harness.h"

#include <cstdint>
#include <string>
#include <string_view>

#include "ruvia/http/detail/json/JsonNumber.h"
#include "ruvia/http/detail/json/JsonSkip.h"
#include "ruvia/http/detail/json/JsonString.h"

namespace {

std::string decodeJson(std::string_view raw, bool& ok) {
    std::string out;
    ok = ruvia::detail::decodeJsonString(raw, out);
    return out;
}

}  // namespace

// --- Number scanning -----------------------------------------------------
RUVIA_TEST(json_number_scan_valid) {
    using ruvia::detail::scanJsonNumberTokenLength;
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("0"), std::size_t(1));
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("123"), std::size_t(3));
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("-0"), std::size_t(2));
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("-123"), std::size_t(4));
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("0.5"), std::size_t(3));
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("-0.5e10"), std::size_t(7));
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("1E+5"), std::size_t(4));
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("12.34e-6"), std::size_t(8));
    // stops at trailing non-number chars, returns consumed length
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("42,rest"), std::size_t(2));
}

RUVIA_TEST(json_number_scan_invalid) {
    using ruvia::detail::scanJsonNumberTokenLength;
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength(""), std::size_t(0));
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("-"), std::size_t(0));
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("01"), std::size_t(0));   // leading zero
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("00"), std::size_t(0));
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("1."), std::size_t(0));   // no fraction digits
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("1e"), std::size_t(0));   // no exponent digits
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("1e+"), std::size_t(0));
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength(".5"), std::size_t(0));   // no integer part
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("abc"), std::size_t(0));
    RUVIA_CHECK_EQ(scanJsonNumberTokenLength("+1"), std::size_t(0));   // leading plus not allowed
}

RUVIA_TEST(json_number_parse_values) {
    {
        std::string_view in = "42";
        int v = 0;
        RUVIA_CHECK(ruvia::detail::parseJsonNumberValue(in, v));
        RUVIA_CHECK_EQ(v, 42);
        RUVIA_CHECK(in.empty());
    }
    {
        std::string_view in = "-3.5e2 tail";
        double v = 0;
        RUVIA_CHECK(ruvia::detail::parseJsonNumberValue(in, v));
        RUVIA_CHECK(v == -350.0);
        RUVIA_CHECK_EQ(in, std::string_view(" tail"));
    }
    {
        std::string_view in = "01";  // invalid leading zero
        int v = 0;
        RUVIA_CHECK(!ruvia::detail::parseJsonNumberValue(in, v));
    }
}

// --- String view parsing -------------------------------------------------
RUVIA_TEST(json_string_view_unescaped) {
    std::string_view in = "\"hello\" rest";
    std::string_view value;
    RUVIA_CHECK(ruvia::detail::parseJsonStringView(in, value));
    RUVIA_CHECK_EQ(value, std::string_view("hello"));
    RUVIA_CHECK_EQ(in, std::string_view(" rest"));
}

RUVIA_TEST(json_string_view_escaped_reports_escape) {
    // parseJsonStringView returns false for a string that needs decoding.
    std::string_view in = "\"a\\nb\"";
    std::string_view value;
    RUVIA_CHECK(!ruvia::detail::parseJsonStringView(in, value));
}

RUVIA_TEST(json_string_raw_flags_escape) {
    std::string_view in = "\"a\\nb\"";
    std::string_view value;
    bool escaped = false;
    RUVIA_CHECK(ruvia::detail::parseJsonStringRaw(in, value, escaped));
    RUVIA_CHECK(escaped);
    RUVIA_CHECK_EQ(value, std::string_view("a\\nb"));
}

RUVIA_TEST(json_string_raw_rejects_control_and_unterminated) {
    {
        std::string_view in = std::string_view("\"a\x01""b\"", 5);  // raw control char
        std::string_view value;
        bool escaped = false;
        RUVIA_CHECK(!ruvia::detail::parseJsonStringRaw(in, value, escaped));
    }
    {
        std::string_view in = "\"unterminated";
        std::string_view value;
        bool escaped = false;
        RUVIA_CHECK(!ruvia::detail::parseJsonStringRaw(in, value, escaped));
    }
    {
        std::string_view in = "\"bad\\x\"";  // invalid escape
        std::string_view value;
        bool escaped = false;
        RUVIA_CHECK(!ruvia::detail::parseJsonStringRaw(in, value, escaped));
    }
}

// --- String decoding (escape expansion) ----------------------------------
RUVIA_TEST(json_decode_simple_escapes) {
    bool ok = false;
    RUVIA_CHECK_EQ(decodeJson("a\\nb\\tc", ok), std::string("a\nb\tc"));
    RUVIA_CHECK(ok);
    RUVIA_CHECK_EQ(decodeJson("quote\\\"slash\\\\fwd\\/", ok), std::string("quote\"slash\\fwd/"));
    RUVIA_CHECK(ok);
    RUVIA_CHECK_EQ(decodeJson("\\b\\f\\r", ok), std::string("\b\f\r"));
    RUVIA_CHECK(ok);
}

RUVIA_TEST(json_decode_bmp_escape) {
    bool ok = false;
    // U+00E9 (é) -> C3 A9
    const std::string r = decodeJson("caf\\u00e9", ok);
    RUVIA_CHECK(ok);
    RUVIA_CHECK_EQ(r, std::string("caf\xC3\xA9"));
    // U+20AC (€) -> E2 82 AC
    const std::string e = decodeJson("\\u20ac", ok);
    RUVIA_CHECK(ok);
    RUVIA_CHECK_EQ(e, std::string("\xE2\x82\xAC"));
}

RUVIA_TEST(json_decode_surrogate_pair) {
    bool ok = false;
    // U+1F600 😀 -> F0 9F 98 80
    const std::string r = decodeJson("\\ud83d\\ude00", ok);
    RUVIA_CHECK(ok);
    RUVIA_CHECK_EQ(r, std::string("\xF0\x9F\x98\x80"));
}

RUVIA_TEST(json_decode_invalid_surrogates) {
    bool ok = true;
    (void)decodeJson("\\ud83d", ok);          // lone high surrogate
    RUVIA_CHECK(!ok);
    ok = true;
    (void)decodeJson("\\ude00", ok);          // lone low surrogate
    RUVIA_CHECK(!ok);
    ok = true;
    (void)decodeJson("\\ud83dx", ok);         // high not followed by \u
    RUVIA_CHECK(!ok);
    ok = true;
    (void)decodeJson("\\ud83d\\ud83d", ok);   // high followed by high
    RUVIA_CHECK(!ok);
}

RUVIA_TEST(json_decode_rejects_bad_escapes) {
    bool ok = true;
    (void)decodeJson("\\x", ok);        // unknown escape
    RUVIA_CHECK(!ok);
    ok = true;
    (void)decodeJson("\\u12", ok);      // truncated \u
    RUVIA_CHECK(!ok);
    ok = true;
    (void)decodeJson("\\u12zz", ok);    // non-hex in \u
    RUVIA_CHECK(!ok);
    ok = true;
    (void)decodeJson("trailing\\", ok); // dangling backslash
    RUVIA_CHECK(!ok);
}

RUVIA_TEST(json_appendUtf8_boundaries) {
    using ruvia::detail::appendUtf8;
    std::string s;
    appendUtf8(s, 0x24);      // $
    RUVIA_CHECK_EQ(s, std::string("\x24"));
    s.clear();
    appendUtf8(s, 0x7FF);     // 2-byte max
    RUVIA_CHECK_EQ(s, std::string("\xDF\xBF"));
    s.clear();
    appendUtf8(s, 0xFFFF);    // 3-byte max
    RUVIA_CHECK_EQ(s, std::string("\xEF\xBF\xBF"));
    s.clear();
    appendUtf8(s, 0x10FFFF);  // 4-byte max
    RUVIA_CHECK_EQ(s, std::string("\xF4\x8F\xBF\xBF"));
}

// --- JSON nesting depth is bounded at the documented kMaxJsonDepth --------
RUVIA_TEST(json_depth_within_documented_limit_accepted) {
    // 40 nested arrays is within kMaxJsonDepth (64). A prior double-increment enforced ~half
    // the limit, wrongly rejecting this.
    std::string arr(40, '[');
    arr.append(40, ']');
    std::string_view arrIn(arr);
    RUVIA_CHECK(ruvia::detail::skipJsonValue(arrIn));
    RUVIA_CHECK(arrIn.empty());

    // Objects nest the same way.
    std::string obj;
    for (int i = 0; i < 40; ++i) {
        obj += "{\"k\":";
    }
    obj += "1";
    obj.append(40, '}');
    std::string_view objIn(obj);
    RUVIA_CHECK(ruvia::detail::skipJsonValue(objIn));
    RUVIA_CHECK(objIn.empty());
}

RUVIA_TEST(json_depth_beyond_limit_rejected) {
    // 200 levels exceeds kMaxJsonDepth (64) and must still be rejected (bounded recursion).
    std::string tooDeep(200, '[');
    tooDeep.append(200, ']');
    std::string_view in(tooDeep);
    RUVIA_CHECK(!ruvia::detail::skipJsonValue(in));
}
