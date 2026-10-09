#include <cmath>
#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/detail/json/json_number.h"
#include "ruvia/web/detail/json/json_skip.h"
#include "ruvia/web/detail/json/json_string.h"
#include "ruvia/web/detail/model/parse/json_parser.h"

#include "test_harness.h"

namespace {

std::optional<std::pmr::string> decode_json(std::string_view raw) {
    return ruvia::detail::decode_json_string(raw, std::pmr::get_default_resource());
}

}  // namespace

// --- Number scanning -----------------------------------------------------
RUVIA_TEST(json_number_scan_valid) {
    using ruvia::detail::scan_json_number_token_length;
    RUVIA_CHECK_EQ(scan_json_number_token_length("0"), std::size_t(1));
    RUVIA_CHECK_EQ(scan_json_number_token_length("123"), std::size_t(3));
    RUVIA_CHECK_EQ(scan_json_number_token_length("-0"), std::size_t(2));
    RUVIA_CHECK_EQ(scan_json_number_token_length("-123"), std::size_t(4));
    RUVIA_CHECK_EQ(scan_json_number_token_length("0.5"), std::size_t(3));
    RUVIA_CHECK_EQ(scan_json_number_token_length("-0.5e10"), std::size_t(7));
    RUVIA_CHECK_EQ(scan_json_number_token_length("1E+5"), std::size_t(4));
    RUVIA_CHECK_EQ(scan_json_number_token_length("12.34e-6"), std::size_t(8));
    // stops at trailing non-number chars, returns consumed length
    RUVIA_CHECK_EQ(scan_json_number_token_length("42,rest"), std::size_t(2));
}

RUVIA_TEST(json_number_scan_invalid) {
    using ruvia::detail::scan_json_number_token_length;
    RUVIA_CHECK_EQ(scan_json_number_token_length(""), std::size_t(0));
    RUVIA_CHECK_EQ(scan_json_number_token_length("-"), std::size_t(0));
    RUVIA_CHECK_EQ(scan_json_number_token_length("01"), std::size_t(0));  // leading zero
    RUVIA_CHECK_EQ(scan_json_number_token_length("00"), std::size_t(0));
    RUVIA_CHECK_EQ(scan_json_number_token_length("1."), std::size_t(0));  // no fraction digits
    RUVIA_CHECK_EQ(scan_json_number_token_length("1e"), std::size_t(0));  // no exponent digits
    RUVIA_CHECK_EQ(scan_json_number_token_length("1e+"), std::size_t(0));
    RUVIA_CHECK_EQ(scan_json_number_token_length(".5"), std::size_t(0));  // no integer part
    RUVIA_CHECK_EQ(scan_json_number_token_length("abc"), std::size_t(0));
    RUVIA_CHECK_EQ(scan_json_number_token_length("+1"), std::size_t(0));  // leading plus not allowed
}

RUVIA_TEST(json_number_parse_values) {
    {
        std::string_view in = "42";
        int v = 0;
        RUVIA_CHECK(ruvia::detail::parse_json_number_value(in, v));
        RUVIA_CHECK_EQ(v, 42);
        RUVIA_CHECK(in.empty());
    }
    {
        std::string_view in = "-3.5e2 tail";
        double v = 0;
        RUVIA_CHECK(ruvia::detail::parse_json_number_value(in, v));
        RUVIA_CHECK(v == -350.0);
        RUVIA_CHECK_EQ(in, std::string_view(" tail"));
    }
    {
        std::string_view in = "01";  // invalid leading zero
        int v = 0;
        RUVIA_CHECK(!ruvia::detail::parse_json_number_value(in, v));
    }
}

RUVIA_TEST(json_number_parse_type_boundaries) {
    using ruvia::detail::parse_json_number_value;

    // An integer target must REJECT a well-formed but non-integer JSON number
    // rather than silently truncate it, and must leave the cursor unmoved so the
    // caller sees the parse failure at the value's start.
    {
        std::string_view in = "1.5";
        int v = -1;
        RUVIA_CHECK(!parse_json_number_value(in, v));
        RUVIA_CHECK_EQ(in, std::string_view("1.5"));  // not consumed
    }
    {
        std::string_view in = "1e2";  // exponent form is not an integer literal
        int v = -1;
        RUVIA_CHECK(!parse_json_number_value(in, v));
    }
    // A negative value cannot fit an unsigned target.
    {
        std::string_view in = "-5";
        unsigned v = 7;
        RUVIA_CHECK(!parse_json_number_value(in, v));
    }
    // Out-of-range magnitudes are rejected, not wrapped/clamped.
    {
        std::string_view in = "99999999999999999999";  // > INT64/INT32 max
        int v = -1;
        RUVIA_CHECK(!parse_json_number_value(in, v));
        RUVIA_CHECK_EQ(in, std::string_view("99999999999999999999"));  // not consumed
    }
    // The same exponent form parses fine into a floating target.
    {
        std::string_view in = "1e2";
        double v = 0;
        RUVIA_CHECK(parse_json_number_value(in, v));
        RUVIA_CHECK(v == 100.0);
        RUVIA_CHECK(in.empty());
    }
    {
        std::string_view in = "1e39";
        float v = 0;
        RUVIA_CHECK(!parse_json_number_value(in, v));
        RUVIA_CHECK_EQ(in, std::string_view("1e39"));
    }
    {
        std::string_view in = "1e-999";
        double v = 0;
        RUVIA_CHECK(!parse_json_number_value(in, v));
        RUVIA_CHECK_EQ(in, std::string_view("1e-999"));
    }
}

// --- String token scanning -----------------------------------------------
RUVIA_TEST(json_string_token_literal) {
    std::string_view in = "\"hello\" rest";
    const auto parsed_value = ruvia::detail::parse_json_string(in);
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK_EQ(parsed_value->raw(), std::string_view("hello"));
    RUVIA_CHECK(parsed_value->encoding() == ruvia::detail::json_string_encoding::literal);
    RUVIA_CHECK_EQ(in, std::string_view(" rest"));
}

RUVIA_TEST(json_string_token_carries_escape_encoding) {
    std::string_view in = "\"a\\nb\"";
    const auto parsed_value = ruvia::detail::parse_json_string(in);
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK_EQ(parsed_value->raw(), std::string_view("a\\nb"));
    RUVIA_CHECK(parsed_value->encoding() == ruvia::detail::json_string_encoding::escaped);
}

RUVIA_TEST(json_string_token_scans_across_simd_boundaries) {
    std::string storage = "\"";
    storage.append(15, 'a');
    storage.append("\xC3\xA9", 2);
    storage.append(14, 'b');
    storage.append("\\n");
    storage.append(16, 'c');
    storage.push_back('"');

    std::string_view input = storage;
    const auto parsed_value = ruvia::detail::parse_json_string(input);
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK(parsed_value->encoding() == ruvia::detail::json_string_encoding::escaped);
    RUVIA_CHECK_EQ(parsed_value->raw().size(), storage.size() - 2);
    RUVIA_CHECK(input.empty());
}

RUVIA_TEST(json_string_scan_failure_preserves_input_cursor) {
    {
        std::string_view in = std::string_view(
            "\"a\x01"
            "b\"",
            5);  // raw control char
        const auto original = in;
        RUVIA_CHECK(!ruvia::detail::parse_json_string(in).has_value());
        RUVIA_CHECK_EQ(in, original);
    }
    {
        std::string_view in = "  \"unterminated";
        const auto original = in;
        RUVIA_CHECK(!ruvia::detail::parse_json_string(in).has_value());
        RUVIA_CHECK_EQ(in, original);
    }
    {
        std::string_view in = "\"bad\\x\"";  // invalid escape
        const auto original = in;
        RUVIA_CHECK(!ruvia::detail::parse_json_string(in).has_value());
        RUVIA_CHECK_EQ(in, original);
    }
}

RUVIA_TEST(json_string_validates_utf8_content) {
    // Valid raw UTF-8 in string content passes through unchanged (2/3/4-byte).
    const auto e_acute = decode_json(std::string_view("caf\xC3\xA9", 5));
    RUVIA_CHECK(e_acute.has_value());
    RUVIA_CHECK_EQ(std::string_view(*e_acute), std::string_view("caf\xC3\xA9", 5));  // é
    const auto euro = decode_json(std::string_view("\xE2\x82\xAC", 3));
    RUVIA_CHECK(euro.has_value());
    RUVIA_CHECK_EQ(std::string_view(*euro), std::string_view("\xE2\x82\xAC", 3));  // €
    const auto smile = decode_json(std::string_view("\xF0\x9F\x98\x80", 4));
    RUVIA_CHECK(smile.has_value());
    RUVIA_CHECK_EQ(std::string_view(*smile), std::string_view("\xF0\x9F\x98\x80", 4));  // U+1F600
    const auto unicode_max = decode_json(std::string_view("\xF4\x8F\xBF\xBF", 4));
    RUVIA_CHECK(unicode_max.has_value());
    RUVIA_CHECK_EQ(
        std::string_view(*unicode_max), std::string_view("\xF4\x8F\xBF\xBF", 4));  // U+10FFFF

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
        RUVIA_CHECK(!decode_json(b).has_value());
    }

    // The validation scan (parse_json_string) applies the same rule.
    {
        std::string_view in = std::string_view("\"\xFF\"", 3);  // invalid lead byte
        const auto original = in;
        RUVIA_CHECK(!ruvia::detail::parse_json_string(in).has_value());
        RUVIA_CHECK_EQ(in, original);
    }
    {
        std::string_view in = std::string_view("\"caf\xC3\xA9\"", 7);  // valid é passes
        const auto parsed_value = ruvia::detail::parse_json_string(in);
        RUVIA_CHECK(parsed_value.has_value());
        RUVIA_CHECK_EQ(std::string(parsed_value->raw()), std::string("caf\xC3\xA9"));
    }
}

// --- String decoding (escape expansion) ----------------------------------
RUVIA_TEST(json_decode_simple_escapes) {
    const auto whitespace = decode_json("a\\nb\\tc");
    RUVIA_CHECK(whitespace.has_value());
    RUVIA_CHECK_EQ(std::string_view(*whitespace), std::string_view("a\nb\tc"));
    const auto punctuation = decode_json("quote\\\"slash\\\\fwd\\/");
    RUVIA_CHECK(punctuation.has_value());
    RUVIA_CHECK_EQ(std::string_view(*punctuation), std::string_view("quote\"slash\\fwd/"));
    const auto controls = decode_json("\\b\\f\\r");
    RUVIA_CHECK(controls.has_value());
    RUVIA_CHECK_EQ(std::string_view(*controls), std::string_view("\b\f\r", 3));
}

RUVIA_TEST(json_decode_bmp_escape) {
    // U+00E9 (é) -> C3 A9
    const auto r = decode_json("caf\\u00e9");
    RUVIA_CHECK(r.has_value());
    RUVIA_CHECK_EQ(std::string_view(*r), std::string_view("caf\xC3\xA9", 5));
    // U+20AC (€) -> E2 82 AC
    const auto e = decode_json("\\u20ac");
    RUVIA_CHECK(e.has_value());
    RUVIA_CHECK_EQ(std::string_view(*e), std::string_view("\xE2\x82\xAC", 3));
}

RUVIA_TEST(json_decode_surrogate_pair) {
    // U+1F600 😀 -> F0 9F 98 80
    const auto r = decode_json("\\ud83d\\ude00");
    RUVIA_CHECK(r.has_value());
    RUVIA_CHECK_EQ(std::string_view(*r), std::string_view("\xF0\x9F\x98\x80", 4));
}

RUVIA_TEST(json_decode_invalid_surrogates) {
    RUVIA_CHECK(!decode_json("\\ud83d").has_value());         // lone high surrogate
    RUVIA_CHECK(!decode_json("\\ude00").has_value());         // lone low surrogate
    RUVIA_CHECK(!decode_json("\\ud83dx").has_value());        // high not followed by \u
    RUVIA_CHECK(!decode_json("\\ud83d\\ud83d").has_value());  // high followed by high
}

RUVIA_TEST(json_string_token_rejects_invalid_surrogate_escapes) {
    const auto rejects = [&](std::string_view raw) {
        std::string_view in = raw;
        const auto original = in;
        RUVIA_CHECK(!ruvia::detail::parse_json_string(in).has_value());
        RUVIA_CHECK_EQ(in, original);
    };

    rejects(R"("\ud83d")");        // lone high surrogate
    rejects(R"("\ude00")");        // lone low surrogate
    rejects(R"("\ud83dx")");       // high not followed by \u
    rejects(R"("\ud83d\ud83d")");  // high followed by high

    std::string_view valid = R"("\ud83d\ude00" tail)";
    const auto parsed_value = ruvia::detail::parse_json_string(valid);
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK_EQ(parsed_value->raw(), std::string_view(R"(\ud83d\ude00)"));
    RUVIA_CHECK_EQ(valid, std::string_view(" tail"));
}

RUVIA_TEST(json_decode_rejects_bad_escapes) {
    RUVIA_CHECK(!decode_json("\\x").has_value());         // unknown escape
    RUVIA_CHECK(!decode_json("\\u12").has_value());       // truncated \u
    RUVIA_CHECK(!decode_json("\\u12zz").has_value());     // non-hex in \u
    RUVIA_CHECK(!decode_json("trailing\\").has_value());  // dangling backslash
}

RUVIA_TEST(json_string_value_failure_preserves_input_cursor) {
    auto* const resource = std::pmr::get_default_resource();

    std::string_view malformed = R"("prefix\ud83d")";
    const auto original = malformed;
    const auto rejected = ruvia::detail::parse_json_value<ruvia::string>(malformed, resource);
    RUVIA_CHECK(!rejected.has_value());
    RUVIA_CHECK_EQ(malformed, original);

    std::string_view valid = R"("decoded\u0020value")";
    const auto parsed_value = ruvia::detail::parse_json_value<ruvia::string>(valid, resource);
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK_EQ(parsed_value->view(), std::string_view("decoded value"));
}

RUVIA_TEST(json_sequence_failure_preserves_input_cursor) {
    auto* const resource = std::pmr::get_default_resource();
    using array_t_type = ruvia::array<ruvia::int32>;
    using list_t_type = ruvia::boxed_array<ruvia::int32>;

    std::string_view malformed_array = R"([1,2,"bad"] tail)";
    const auto original_array = malformed_array;
    const auto rejected_array = ruvia::detail::parse_json_value<array_t_type>(malformed_array, resource);
    RUVIA_CHECK(!rejected_array.has_value());
    RUVIA_CHECK_EQ(malformed_array, original_array);

    std::string_view valid_array = "[1,2,3] tail";
    const auto parsed_array = ruvia::detail::parse_json_value<array_t_type>(valid_array, resource);
    RUVIA_CHECK(parsed_array.has_value());
    RUVIA_CHECK_EQ(parsed_array->size(), std::size_t{3});
    RUVIA_CHECK_EQ(static_cast<std::int32_t>((*parsed_array)[2]), 3);
    RUVIA_CHECK_EQ(valid_array, std::string_view(" tail"));

    std::string_view malformed_list = "[4,false] tail";
    const auto original_list = malformed_list;
    const auto rejected_list = ruvia::detail::parse_json_value<list_t_type>(malformed_list, resource);
    RUVIA_CHECK(!rejected_list.has_value());
    RUVIA_CHECK_EQ(malformed_list, original_list);

    std::string_view valid_list = "[4,5] tail";
    const auto parsed_list = ruvia::detail::parse_json_value<list_t_type>(valid_list, resource);
    RUVIA_CHECK(parsed_list.has_value());
    RUVIA_CHECK_EQ(parsed_list->size(), std::size_t{2});
    RUVIA_CHECK_EQ(static_cast<std::int32_t>((*parsed_list)[1]), 5);
    RUVIA_CHECK_EQ(valid_list, std::string_view(" tail"));
}

RUVIA_TEST(json_append_utf8_boundaries) {
    using ruvia::detail::append_utf8;
    std::string s;
    append_utf8(s, 0x24);  // $
    RUVIA_CHECK_EQ(s, std::string("\x24"));
    s.clear();
    append_utf8(s, 0x7FF);  // 2-byte max
    RUVIA_CHECK_EQ(s, std::string("\xDF\xBF"));
    s.clear();
    append_utf8(s, 0xFFFF);  // 3-byte max
    RUVIA_CHECK_EQ(s, std::string("\xEF\xBF\xBF"));
    s.clear();
    append_utf8(s, 0x10FFFF);  // 4-byte max
    RUVIA_CHECK_EQ(s, std::string("\xF4\x8F\xBF\xBF"));
}

// --- JSON nesting depth is bounded at the documented max_json_depth --------
RUVIA_TEST(json_depth_within_documented_limit_accepted) {
    // 40 nested arrays is within max_json_depth (64). A prior double-increment enforced ~half
    // the limit, wrongly rejecting this.
    std::string arr(40, '[');
    arr.append(40, ']');
    std::string_view arr_in(arr);
    RUVIA_CHECK(ruvia::detail::skip_json_value(arr_in));
    RUVIA_CHECK(arr_in.empty());

    // Objects nest the same way.
    std::string obj;
    for (int i = 0; i < 40; ++i) {
        obj += "{\"k\":";
    }
    obj += "1";
    obj.append(40, '}');
    std::string_view obj_in(obj);
    RUVIA_CHECK(ruvia::detail::skip_json_value(obj_in));
    RUVIA_CHECK(obj_in.empty());
}

RUVIA_TEST(json_depth_beyond_limit_rejected) {
    // 200 levels exceeds max_json_depth (64) and must still be rejected (bounded recursion).
    std::string too_deep(200, '[');
    too_deep.append(200, ']');
    std::string_view in(too_deep);
    RUVIA_CHECK(!ruvia::detail::skip_json_value(in));

    // Objects have their own depth guard (skip_json_object), a distinct code path
    // from arrays: a deeply nested object must be rejected too, or a stack-overflow
    // DoS reopens through the object branch alone.
    std::string deep_obj;
    for (int i = 0; i < 200; ++i) {
        deep_obj += "{\"k\":";
    }
    deep_obj += "1";
    deep_obj.append(200, '}');
    std::string_view obj_in(deep_obj);
    RUVIA_CHECK(!ruvia::detail::skip_json_value(obj_in));
}

RUVIA_TEST(json_number_float_uses_target_precision_and_range) {
    std::string_view input = "1.000000059604644775390626,";
    float value = 0;
    RUVIA_CHECK(ruvia::detail::parse_json_number_value(input, value));
    RUVIA_CHECK_EQ(value, std::nextafter(1.0f, 2.0f));
    RUVIA_CHECK_EQ(input, std::string_view(","));
    for (const auto text : {"1e-46", "1e39"}) {
        input = text;
        value = 7;
        RUVIA_CHECK(!ruvia::detail::parse_json_number_value(input, value));
        RUVIA_CHECK_EQ(input, std::string_view(text));
        RUVIA_CHECK_EQ(value, 7.0f);
    }
}
