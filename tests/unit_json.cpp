#include "test_harness.h"

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>

#include "ruvia/web/detail/json/JsonNumber.h"
#include "ruvia/web/detail/json/JsonSkip.h"
#include "ruvia/web/detail/json/JsonString.h"
#include "ruvia/web/detail/model/JsonParser.h"

namespace {

std::optional<std::pmr::string> decodeJson(std::string_view raw) {
    return ruvia::detail::decodeJsonString(
        raw,
        std::pmr::get_default_resource());
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

RUVIA_TEST(json_number_parse_type_boundaries) {
    using ruvia::detail::parseJsonNumberValue;

    // An integer target must REJECT a well-formed but non-integer JSON number
    // rather than silently truncate it, and must leave the cursor unmoved so the
    // caller sees the parse failure at the value's start.
    {
        std::string_view in = "1.5";
        int v = -1;
        RUVIA_CHECK(!parseJsonNumberValue(in, v));
        RUVIA_CHECK_EQ(in, std::string_view("1.5"));  // not consumed
    }
    {
        std::string_view in = "1e2";  // exponent form is not an integer literal
        int v = -1;
        RUVIA_CHECK(!parseJsonNumberValue(in, v));
    }
    // A negative value cannot fit an unsigned target.
    {
        std::string_view in = "-5";
        unsigned v = 7;
        RUVIA_CHECK(!parseJsonNumberValue(in, v));
    }
    // Out-of-range magnitudes are rejected, not wrapped/clamped.
    {
        std::string_view in = "99999999999999999999";  // > INT64/INT32 max
        int v = -1;
        RUVIA_CHECK(!parseJsonNumberValue(in, v));
        RUVIA_CHECK_EQ(in, std::string_view("99999999999999999999"));  // not consumed
    }
    // The same exponent form parses fine into a floating target.
    {
        std::string_view in = "1e2";
        double v = 0;
        RUVIA_CHECK(parseJsonNumberValue(in, v));
        RUVIA_CHECK(v == 100.0);
        RUVIA_CHECK(in.empty());
    }
}

// --- String token scanning -----------------------------------------------
RUVIA_TEST(json_string_token_literal) {
    std::string_view in = "\"hello\" rest";
    const auto parsed = ruvia::detail::parseJsonString(in);
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK_EQ(parsed->raw(), std::string_view("hello"));
    RUVIA_CHECK(parsed->encoding() == ruvia::detail::JsonStringEncoding::kLiteral);
    RUVIA_CHECK_EQ(in, std::string_view(" rest"));
}

RUVIA_TEST(json_string_token_carries_escape_encoding) {
    std::string_view in = "\"a\\nb\"";
    const auto parsed = ruvia::detail::parseJsonString(in);
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK_EQ(parsed->raw(), std::string_view("a\\nb"));
    RUVIA_CHECK(parsed->encoding() == ruvia::detail::JsonStringEncoding::kEscaped);
}

RUVIA_TEST(json_string_scan_failure_preserves_input_cursor) {
    {
        std::string_view in = std::string_view("\"a\x01""b\"", 5);  // raw control char
        const auto original = in;
        RUVIA_CHECK(!ruvia::detail::parseJsonString(in).has_value());
        RUVIA_CHECK_EQ(in, original);
    }
    {
        std::string_view in = "  \"unterminated";
        const auto original = in;
        RUVIA_CHECK(!ruvia::detail::parseJsonString(in).has_value());
        RUVIA_CHECK_EQ(in, original);
    }
    {
        std::string_view in = "\"bad\\x\"";  // invalid escape
        const auto original = in;
        RUVIA_CHECK(!ruvia::detail::parseJsonString(in).has_value());
        RUVIA_CHECK_EQ(in, original);
    }
}

RUVIA_TEST(json_string_validates_utf8_content) {
    // Valid raw UTF-8 in string content passes through unchanged (2/3/4-byte).
    const auto eAcute = decodeJson(std::string_view("caf\xC3\xA9", 5));
    RUVIA_CHECK(eAcute.has_value());
    RUVIA_CHECK_EQ(std::string_view(*eAcute), std::string_view("caf\xC3\xA9", 5));  // é
    const auto euro = decodeJson(std::string_view("\xE2\x82\xAC", 3));
    RUVIA_CHECK(euro.has_value());
    RUVIA_CHECK_EQ(std::string_view(*euro), std::string_view("\xE2\x82\xAC", 3));  // €
    const auto smile = decodeJson(std::string_view("\xF0\x9F\x98\x80", 4));
    RUVIA_CHECK(smile.has_value());
    RUVIA_CHECK_EQ(std::string_view(*smile), std::string_view("\xF0\x9F\x98\x80", 4));  // U+1F600
    const auto unicodeMax = decodeJson(std::string_view("\xF4\x8F\xBF\xBF", 4));
    RUVIA_CHECK(unicodeMax.has_value());
    RUVIA_CHECK_EQ(std::string_view(*unicodeMax), std::string_view("\xF4\x8F\xBF\xBF", 4));  // U+10FFFF

    // Ill-formed UTF-8 is rejected (RFC 8259 §8.1 / Unicode Table 3-7).
    const std::string_view bad[] = {
        std::string_view("\x80", 1),              // bare continuation byte
        std::string_view("\xC1\x80", 2),          // overlong 2-byte lead (C0/C1)
        std::string_view("\xE0\x80\x80", 3),      // overlong 3-byte (E0 80..9F)
        std::string_view("\xED\xA0\x80", 3),      // UTF-16 surrogate U+D800
        std::string_view("\xF0\x80\x80\x80", 4),  // overlong 4-byte (F0 80..8F)
        std::string_view("\xF4\x90\x80\x80", 4),  // above U+10FFFF
        std::string_view("\xF5\x80\x80\x80", 4),  // invalid lead >= F5
        std::string_view("\xE2\x82", 2),          // truncated 3-byte
        std::string_view("\xC3", 1),              // truncated 2-byte
    };
    for (const auto b : bad) {
        RUVIA_CHECK(!decodeJson(b).has_value());
    }

    // The validation scan (parseJsonString) applies the same rule.
    {
        std::string_view in = std::string_view("\"\xFF\"", 3);  // invalid lead byte
        const auto original = in;
        RUVIA_CHECK(!ruvia::detail::parseJsonString(in).has_value());
        RUVIA_CHECK_EQ(in, original);
    }
    {
        std::string_view in = std::string_view("\"caf\xC3\xA9\"", 7);  // valid é passes
        const auto parsed = ruvia::detail::parseJsonString(in);
        RUVIA_CHECK(parsed.has_value());
        RUVIA_CHECK_EQ(std::string(parsed->raw()), std::string("caf\xC3\xA9"));
    }
}

// --- String decoding (escape expansion) ----------------------------------
RUVIA_TEST(json_decode_simple_escapes) {
    const auto whitespace = decodeJson("a\\nb\\tc");
    RUVIA_CHECK(whitespace.has_value());
    RUVIA_CHECK_EQ(std::string_view(*whitespace), std::string_view("a\nb\tc"));
    const auto punctuation = decodeJson("quote\\\"slash\\\\fwd\\/");
    RUVIA_CHECK(punctuation.has_value());
    RUVIA_CHECK_EQ(std::string_view(*punctuation), std::string_view("quote\"slash\\fwd/"));
    const auto controls = decodeJson("\\b\\f\\r");
    RUVIA_CHECK(controls.has_value());
    RUVIA_CHECK_EQ(std::string_view(*controls), std::string_view("\b\f\r", 3));
}

RUVIA_TEST(json_decode_bmp_escape) {
    // U+00E9 (é) -> C3 A9
    const auto r = decodeJson("caf\\u00e9");
    RUVIA_CHECK(r.has_value());
    RUVIA_CHECK_EQ(std::string_view(*r), std::string_view("caf\xC3\xA9", 5));
    // U+20AC (€) -> E2 82 AC
    const auto e = decodeJson("\\u20ac");
    RUVIA_CHECK(e.has_value());
    RUVIA_CHECK_EQ(std::string_view(*e), std::string_view("\xE2\x82\xAC", 3));
}

RUVIA_TEST(json_decode_surrogate_pair) {
    // U+1F600 😀 -> F0 9F 98 80
    const auto r = decodeJson("\\ud83d\\ude00");
    RUVIA_CHECK(r.has_value());
    RUVIA_CHECK_EQ(std::string_view(*r), std::string_view("\xF0\x9F\x98\x80", 4));
}

RUVIA_TEST(json_decode_invalid_surrogates) {
    RUVIA_CHECK(!decodeJson("\\ud83d").has_value());          // lone high surrogate
    RUVIA_CHECK(!decodeJson("\\ude00").has_value());          // lone low surrogate
    RUVIA_CHECK(!decodeJson("\\ud83dx").has_value());         // high not followed by \u
    RUVIA_CHECK(!decodeJson("\\ud83d\\ud83d").has_value()); // high followed by high
}

RUVIA_TEST(json_decode_rejects_bad_escapes) {
    RUVIA_CHECK(!decodeJson("\\x").has_value());        // unknown escape
    RUVIA_CHECK(!decodeJson("\\u12").has_value());      // truncated \u
    RUVIA_CHECK(!decodeJson("\\u12zz").has_value());    // non-hex in \u
    RUVIA_CHECK(!decodeJson("trailing\\").has_value()); // dangling backslash
}

RUVIA_TEST(json_string_decode_failure_preserves_existing_model_value) {
    auto* const resource = std::pmr::get_default_resource();
    ruvia::String value("original", resource);

    std::string_view malformed = R"("prefix\ud83d")";
    RUVIA_CHECK(!ruvia::detail::parseJsonValue(malformed, value, resource));
    RUVIA_CHECK_EQ(value.view(), std::string_view("original"));

    std::string_view valid = R"("decoded\u0020value")";
    RUVIA_CHECK(ruvia::detail::parseJsonValue(valid, value, resource));
    RUVIA_CHECK_EQ(value.view(), std::string_view("decoded value"));
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

    // Objects have their own depth guard (skipJsonObject), a distinct code path
    // from arrays: a deeply nested object must be rejected too, or a stack-overflow
    // DoS reopens through the object branch alone.
    std::string deepObj;
    for (int i = 0; i < 200; ++i) {
        deepObj += "{\"k\":";
    }
    deepObj += "1";
    deepObj.append(200, '}');
    std::string_view objIn(deepObj);
    RUVIA_CHECK(!ruvia::detail::skipJsonValue(objIn));
}
