#include <gtest/gtest.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/http_request.h"
#include "ruvia/web/app.h"
#include "ruvia/web/controller.h"
#include "ruvia/web/model.h"

#include "test_exceptions.h"

namespace ruvia::test {

RUVIA_MODEL(accessor_surface_request, RUVIA_OPTIONAL_FIELD(message, ruvia::string));

RUVIA_MODEL(nested_model_item, RUVIA_REQUIRED_FIELD(id, ruvia::uint32),
    RUVIA_OPTIONAL_FIELD(label, ruvia::string));

RUVIA_MODEL(nested_model_envelope, RUVIA_REQUIRED_FIELD(primary, nested_model_item),
    RUVIA_REQUIRED_FIELD(items, ruvia::array<nested_model_item>),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>));

RUVIA_MODEL(nested_response_item, RUVIA_REQUIRED_FIELD(id, ruvia::uint32),
    RUVIA_OPTIONAL_FIELD(label, ruvia::string));

RUVIA_MODEL(nested_response_envelope, RUVIA_REQUIRED_FIELD(primary, nested_response_item),
    RUVIA_REQUIRED_FIELD(items, ruvia::array<nested_response_item>),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>));

#define RUVIA_TEST_BOOL_FIELD(field) RUVIA_OPTIONAL_FIELD(field, ruvia::bool_value)
RUVIA_MODEL(unlimited_field_count_response, RUVIA_TEST_BOOL_FIELD(f01),
    RUVIA_TEST_BOOL_FIELD(f02), RUVIA_TEST_BOOL_FIELD(f03), RUVIA_TEST_BOOL_FIELD(f04),
    RUVIA_TEST_BOOL_FIELD(f05), RUVIA_TEST_BOOL_FIELD(f06), RUVIA_TEST_BOOL_FIELD(f07),
    RUVIA_TEST_BOOL_FIELD(f08), RUVIA_TEST_BOOL_FIELD(f09), RUVIA_TEST_BOOL_FIELD(f10),
    RUVIA_TEST_BOOL_FIELD(f11), RUVIA_TEST_BOOL_FIELD(f12), RUVIA_TEST_BOOL_FIELD(f13),
    RUVIA_TEST_BOOL_FIELD(f14), RUVIA_TEST_BOOL_FIELD(f15), RUVIA_TEST_BOOL_FIELD(f16),
    RUVIA_TEST_BOOL_FIELD(f17), RUVIA_TEST_BOOL_FIELD(f18), RUVIA_TEST_BOOL_FIELD(f19),
    RUVIA_TEST_BOOL_FIELD(f20), RUVIA_TEST_BOOL_FIELD(f21), RUVIA_TEST_BOOL_FIELD(f22),
    RUVIA_TEST_BOOL_FIELD(f23), RUVIA_TEST_BOOL_FIELD(f24), RUVIA_TEST_BOOL_FIELD(f25),
    RUVIA_TEST_BOOL_FIELD(f26), RUVIA_TEST_BOOL_FIELD(f27), RUVIA_TEST_BOOL_FIELD(f28),
    RUVIA_TEST_BOOL_FIELD(f29), RUVIA_TEST_BOOL_FIELD(f30), RUVIA_TEST_BOOL_FIELD(f31),
    RUVIA_TEST_BOOL_FIELD(f32), RUVIA_TEST_BOOL_FIELD(f33), RUVIA_TEST_BOOL_FIELD(f34),
    RUVIA_TEST_BOOL_FIELD(f35), RUVIA_TEST_BOOL_FIELD(f36), RUVIA_TEST_BOOL_FIELD(f37),
    RUVIA_TEST_BOOL_FIELD(f38), RUVIA_TEST_BOOL_FIELD(f39), RUVIA_TEST_BOOL_FIELD(f40),
    RUVIA_TEST_BOOL_FIELD(f41), RUVIA_TEST_BOOL_FIELD(f42), RUVIA_TEST_BOOL_FIELD(f43),
    RUVIA_TEST_BOOL_FIELD(f44), RUVIA_TEST_BOOL_FIELD(f45), RUVIA_TEST_BOOL_FIELD(f46),
    RUVIA_TEST_BOOL_FIELD(f47), RUVIA_TEST_BOOL_FIELD(f48), RUVIA_TEST_BOOL_FIELD(f49),
    RUVIA_TEST_BOOL_FIELD(f50), RUVIA_TEST_BOOL_FIELD(f51), RUVIA_TEST_BOOL_FIELD(f52),
    RUVIA_TEST_BOOL_FIELD(f53), RUVIA_TEST_BOOL_FIELD(f54), RUVIA_TEST_BOOL_FIELD(f55),
    RUVIA_TEST_BOOL_FIELD(f56), RUVIA_TEST_BOOL_FIELD(f57), RUVIA_TEST_BOOL_FIELD(f58),
    RUVIA_TEST_BOOL_FIELD(f59), RUVIA_TEST_BOOL_FIELD(f60), RUVIA_TEST_BOOL_FIELD(f61),
    RUVIA_TEST_BOOL_FIELD(f62), RUVIA_TEST_BOOL_FIELD(f63), RUVIA_TEST_BOOL_FIELD(f64),
    RUVIA_TEST_BOOL_FIELD(f65));
#undef RUVIA_TEST_BOOL_FIELD

}  // namespace ruvia::test

namespace {

std::size_t default_evaluations = 0;
std::size_t predicate_evaluations = 0;
std::size_t nested_default_evaluations = 0;

std::string nested_default() {
    if (++nested_default_evaluations == 2) {
        throw std::runtime_error("array default failed");
    }
    return std::string(256, 'd');
}

int codec_default() {
    ++default_evaluations;
    return 7;
}

bool counted_predicate(const ruvia::string& value) {
    ++predicate_evaluations;
    return value.view() == "valid";
}

RUVIA_MODEL(codec_escaped_keys,
    RUVIA_REQUIRED_FIELD_NAME("caf\xC3\xA9", accented, ruvia::int32),
    RUVIA_REQUIRED_FIELD_NAME("tab\tkey", tabbed, ruvia::int32),
    RUVIA_REQUIRED_FIELD_NAME("ctl\x1F", control, ruvia::int32));

RUVIA_MODEL(codec_leaf,
    RUVIA_REQUIRED_FIELD_NAME("wire\"id", id, ruvia::uint64),
    RUVIA_OPTIONAL_FIELD(text, ruvia::string));
RUVIA_MODEL(codec_node,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(children, ruvia::boxed_array<codec_node>));
RUVIA_MODEL(codec_envelope,
    RUVIA_REQUIRED_FIELD(single, codec_leaf),
    RUVIA_REQUIRED_FIELD(array, ruvia::array<codec_leaf>),
    RUVIA_REQUIRED_FIELD(boxed, ruvia::boxed_array<codec_leaf>),
    RUVIA_REQUIRED_FIELD(tree, codec_node));
RUVIA_MODEL(codec_presence,
    RUVIA_REQUIRED_FIELD(required, ruvia::string),
    RUVIA_OPTIONAL_FIELD(nullable, ruvia::string, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(defaulted, ruvia::int32, RUVIA_DEFAULT(codec_default())),
    RUVIA_OPTIONAL_FIELD(emitted, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL),
    RUVIA_OPTIONAL_FIELD(empty, ruvia::string, RUVIA_OMIT_EMPTY));
RUVIA_MODEL(codec_rules,
    RUVIA_REQUIRED_FIELD(value, ruvia::string, RUVIA_CUSTOM(counted_predicate, "not valid")));
RUVIA_MODEL(codec_dynamic,
    RUVIA_REQUIRED_FIELD(value, ruvia::json_value),
    RUVIA_REQUIRED_FIELD(object, ruvia::json_object));
RUVIA_MODEL(codec_dynamic_sequences,
    RUVIA_REQUIRED_FIELD(dense, ruvia::array<ruvia::json_value>),
    RUVIA_REQUIRED_FIELD(boxed, ruvia::boxed_array<ruvia::json_value>));
RUVIA_MODEL(codec_empty);
RUVIA_MODEL(codec_root_nested,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_REQUIRED_FIELD(values, ruvia::array<ruvia::string>));
RUVIA_MODEL(codec_root_types,
    RUVIA_REQUIRED_FIELD(child, codec_root_nested),
    RUVIA_REQUIRED_FIELD(flag, ruvia::bool_value));
RUVIA_MODEL(codec_array_default,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_DEFAULT(nested_default())));

struct forward_branch;
RUVIA_MODEL(forward_root,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(branches, ruvia::array<forward_branch>));
RUVIA_MODEL(forward_branch,
    RUVIA_REQUIRED_FIELD(name, ruvia::string),
    RUVIA_OPTIONAL_FIELD(roots, ruvia::boxed_array<forward_root>));

}  // namespace

TEST(Ruvia, model_json_codec_supports_public_scalar_and_array_roots) {
    auto string_value = ruvia::from_json<ruvia::string>(R"("escaped\\\"text")");
    EXPECT_EQ(string_value.view(), std::string_view("escaped\\\"text"));
    auto integer = ruvia::from_json<ruvia::int64>("-9223372036854775808");
    EXPECT_EQ(integer.value_, INT64_MIN);
    EXPECT_EQ(std::string_view(ruvia::to_json(integer)), "-9223372036854775808");
    auto boolean = ruvia::from_json<ruvia::bool_value>("true");
    EXPECT_TRUE(boolean.value_);
    auto strings = ruvia::from_json<ruvia::array<ruvia::string>>(R"(["a","b"])");
    EXPECT_EQ(strings.size(), std::size_t{2});
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<ruvia::boxed_array<ruvia::string>>(R"(["x",[1]])");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::int64>("1 trailing"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::string>("null"); }));
}

TEST(Ruvia, model_json_scalar_roots_keep_short_output_in_inline_storage) {
    {
        const auto number = ruvia::to_json(ruvia::float_value(1.5f));
        const auto wide_number = ruvia::to_json(ruvia::double_value(2.25));
        const auto nonfinite = ruvia::to_json(ruvia::double_value(std::numeric_limits<double>::infinity()));
        const auto integer = ruvia::to_json(ruvia::int64(7));
        EXPECT_EQ(number, "1.5");
        EXPECT_EQ(wide_number, "2.25");
        EXPECT_EQ(nonfinite, "null");
        EXPECT_EQ(integer, "7");
    }
    {
        const auto encoded = ruvia::to_json(ruvia::double_value(std::numeric_limits<double>::max()));
        EXPECT_EQ(ruvia::from_json<ruvia::double_value>(encoded).value_, std::numeric_limits<double>::max());
    }
}

TEST(Ruvia, model_json_writer_replaces_ill_formed_utf8_with_replacement_characters) {
    // Each literal piece ends at a hex escape so the next letter is not read as a
    // further hex digit.
    std::string input("a\xD3\xC9");            // GBK lead/trail bytes: two maximal subparts
    input += "b\xE4\xBD";                      // truncated 3-byte sequence: one subpart
    input += "c\xC0\xAF";                      // overlong lead and stray continuation
    input += "d\xED\xA0\x80";                  // UTF-16 surrogate code point
    input += "e\xF4\x90\x80\x80";              // above U+10FFFF
    input += "f\xE4\xB8\xAD\xF0\x9F\x98\x80";  // well-formed U+4E2D and U+1F600
    input += "\"\n\xF0\x9F\x98";               // escapes, then a sequence cut at the end
    const std::string replacement("\xEF\xBF\xBD");
    std::string expected(std::string("\"a") + replacement + replacement);
    expected += std::string("b") + replacement;
    expected += std::string("c") + replacement + replacement;
    expected += std::string("d") + replacement + replacement + replacement;
    expected += std::string("e") + replacement + replacement + replacement + replacement;
    expected += "f\xE4\xB8\xAD\xF0\x9F\x98\x80";
    expected += std::string("\\\"\\n") + replacement + "\"";

    const auto output = ruvia::to_json(ruvia::string(input));
    EXPECT_EQ(std::string_view(output), std::string_view(expected));
    // The strict parser rejects ill-formed UTF-8, so a round trip proves the
    // output is well-formed.
    const auto reparsed = ruvia::from_json<ruvia::string>(output);
    EXPECT_TRUE(!reparsed.empty());

    ruvia::array<ruvia::string> nested;
    nested.emplace_back(std::string_view("\xFF"));
    EXPECT_EQ(std::string_view(ruvia::to_json(nested)), std::string_view(std::string("[\"") + replacement + "\"]"));
}

TEST(Ruvia, model_json_member_keys_escape_controls_and_keep_utf8) {
    codec_escaped_keys value;
    value.set<"accented">(1).set<"tabbed">(2).set<"control">(3);
    const auto output = ruvia::to_json(value);
    EXPECT_EQ(std::string_view(output),
        std::string_view("{\"caf\xC3\xA9\":1,\"tab\\tkey\":2,\"ctl\\u001f\":3}"));
    const auto reparsed = ruvia::from_json<codec_escaped_keys>(output);
    EXPECT_TRUE(reparsed == value);
}

TEST(Ruvia, model_json_codec_rejects_nested_wrong_null_and_duplicate_fields) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_root_types>(R"({"child":{"name":null,"values":[]},"flag":true})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_root_types>(R"({"child":{"name":"x","values":[1]},"flag":true})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_root_types>(R"({"child":{"name":"x","values":[]},"flag":true,"flag":false})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_root_types>(R"({"child":{"name":"x","values":[]},"flag":true}junk)");
    }));
}

TEST(Ruvia, model_json_codec_root_model_arrays_validate_and_roundtrip_recursively) {
    constexpr auto input = R"([{"name":"root","children":[{"name":"child"}]}])";
    auto dense = ruvia::from_json<ruvia::array<codec_node>>(input);
    auto boxed = ruvia::from_json<ruvia::boxed_array<codec_node>>(input);
    EXPECT_EQ(std::string_view(ruvia::to_json(dense)), input);
    EXPECT_EQ(std::string_view(ruvia::to_json(boxed)), input);
    for (const auto bad : {R"([{}])", R"([{"name":null}])",
             R"([{"name":"root","children":[{}]}])", R"([{"name":"a","name":"b"}])"}) {
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::array<codec_node>>(bad); }));
        EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<ruvia::boxed_array<codec_node>>(bad); }));
    }
}

TEST(Ruvia, model_json_codec_forward_declared_graph_owns_nested_destination_values) {
    forward_root retained;
    constexpr std::string_view input =
        R"({"name":"root","branches":[{"name":"branch","roots":[{"name":"leaf"}]}]})";
    {
        auto parsed_value = ruvia::from_json<forward_root>(input);
        retained = std::move(parsed_value);
    }
    EXPECT_EQ(std::string_view(ruvia::to_json(retained)), input);
}

TEST(Ruvia, model_json_codec_array_default_exception_releases_previous_elements) {
    const std::string input("[{\"name\":\"" + std::string(256, 'a') +
                            "\"},{\"name\":\"" + std::string(256, 'b') + "\"}]");
    const auto check = [&]<typename array_t_type>() {
        nested_default_evaluations = 0;
        bool threw = false;
        try {
            (void)ruvia::from_json<array_t_type>(input);
        } catch (const std::runtime_error& error) {
            threw = std::string_view(error.what()) == "array default failed";
        }
        EXPECT_TRUE(threw);
        EXPECT_EQ(nested_default_evaluations, std::size_t{2});
    };
    check.template operator()<ruvia::array<codec_array_default>>();
    check.template operator()<ruvia::boxed_array<codec_array_default>>();
}

TEST(Ruvia, model_json_codec_owned_root_string_survives_input_destruction) {
    std::string input(R"("retained")");
    auto parsed_value = ruvia::from_json<ruvia::string>(input);
    input.assign(input.size(), 'x');
    EXPECT_EQ(parsed_value.view(), "retained");
}

TEST(Ruvia, model_json_codec_roundtrips_nested_and_recursive_models) {
    constexpr std::string_view input =
        R"({"single":{"wire\"id":18446744073709551615,"text":"a\\b\"c"},"array":[{"wire\"id":1}],"boxed":[{"wire\"id":2,"text":"two"}],"tree":{"name":"root","children":[{"name":"leaf","children":[{"name":"end"}]}]}})";
    auto parsed_value = ruvia::from_json<codec_envelope>(input);
    const auto& single = parsed_value.get<"single">();
    const auto& tree = parsed_value.get<"tree">();
    EXPECT_EQ(single.get<"id">().value_, std::uint64_t{18446744073709551615ULL});
    EXPECT_EQ(single.get<"text">()->view(), "a\\b\"c");
    EXPECT_EQ(parsed_value.get<"array">().size(), std::size_t{1});
    EXPECT_EQ(parsed_value.get<"boxed">().size(), std::size_t{1});
    EXPECT_EQ(tree.get<"children">()->front().get<"name">().view(), "leaf");

    const auto output = ruvia::to_json(parsed_value);
    EXPECT_EQ(std::string_view(output), input);
    auto reparsed = ruvia::from_json<codec_envelope>(output);
    EXPECT_EQ(reparsed.get<"single">().get<"id">().value_,
        std::uint64_t{18446744073709551615ULL});
    EXPECT_EQ(reparsed.get<"tree">().get<"children">()->front().get<"name">().view(),
        "leaf");
}

TEST(Ruvia, model_json_codec_handles_presence_defaults_and_serialization_options) {
    default_evaluations = 0;
    auto missing = ruvia::from_json<codec_presence>(R"({"required":"x"})");
    EXPECT_EQ(default_evaluations, std::size_t{1});
    EXPECT_TRUE(!missing.is_present<"defaulted">());
    EXPECT_TRUE(!missing.is_present<"nullable">());
    EXPECT_EQ(std::string_view(ruvia::to_json(missing)),
        std::string_view(R"({"required":"x","defaulted":7,"emitted":null})"));
    EXPECT_EQ(default_evaluations, std::size_t{1});

    const auto serialized = ruvia::to_json(missing);
    const std::string roundtrip_input(serialized);
    auto roundtrip = ruvia::from_json<codec_presence>(roundtrip_input);
    EXPECT_TRUE(roundtrip.is_present<"defaulted">());
    EXPECT_EQ(std::string_view(ruvia::to_json(roundtrip)),
        std::string_view(R"({"required":"x","defaulted":7,"emitted":null})"));

    auto values = ruvia::from_json<codec_presence>(
        R"({"required":"x","nullable":null,"empty":""})");
    EXPECT_TRUE(values.is_present<"nullable">() && values.is_null<"nullable">());
    EXPECT_EQ(std::string_view(ruvia::to_json(values)),
        std::string_view(R"({"required":"x","nullable":null,"defaulted":7,"emitted":null})"));
}

TEST(Ruvia, model_json_codec_validates_on_parse_and_serializes_without_running_validation_rules) {
    predicate_evaluations = 0;
    bool rule_rejected = false;
    try {
        (void)ruvia::from_json<codec_rules>(R"({"value":"invalid"})");
    } catch (const ruvia::http_error&) {
        rule_rejected = true;
    }
    EXPECT_TRUE(rule_rejected);
    EXPECT_EQ(predicate_evaluations, std::size_t{1});

    auto parsed_value = ruvia::from_json<codec_rules>(R"({"value":"valid"})");
    EXPECT_EQ(predicate_evaluations, std::size_t{2});
    parsed_value.set<"value">("invalid");
    EXPECT_EQ(std::string_view(ruvia::to_json(parsed_value)), R"({"value":"invalid"})");
    EXPECT_EQ(predicate_evaluations, std::size_t{2});
}

TEST(Ruvia, model_json_codec_preserves_default_null_dynamic_array_elements) {
    const auto check = [](
                           std::initializer_list<std::string_view> tokens, std::string_view expected_array) {
        ruvia::array<ruvia::json_value> dense;
        ruvia::boxed_array<ruvia::json_value> boxed;
        for (const auto token : tokens) {
            if (token == "null") {
                dense.emplace_back();
                boxed.emplace_back();
            } else {
                const auto scalar = ruvia::json_value::parse(token);
                dense.push_back(scalar);
                boxed.push_back(scalar);
            }
        }
        codec_dynamic_sequences value;
        value.set<"dense">(std::move(dense));
        value.set<"boxed">(std::move(boxed));
        const auto output = ruvia::to_json(value);
        const auto expected = "{\"dense\":" + std::string(expected_array) +
                              ",\"boxed\":" + std::string(expected_array) + "}";
        EXPECT_EQ(std::string_view(output), std::string_view(expected));
        const auto reparsed = ruvia::from_json<codec_dynamic_sequences>(output);
        EXPECT_EQ(reparsed.get<"dense">().size(), tokens.size());
        EXPECT_EQ(reparsed.get<"boxed">().size(), tokens.size());
        if (reparsed.get<"dense">().size() != tokens.size() ||
            reparsed.get<"boxed">().size() != tokens.size()) {
            return;
        }
        std::size_t index = 0;
        for (const auto token : tokens) {
            EXPECT_EQ(reparsed.get<"dense">()[index].view(), token);
            EXPECT_EQ(reparsed.get<"boxed">()[index].view(), token);
            ++index;
        }
    };
    check({"null"}, "[null]");
    check({"null", "null", "null"}, "[null,null,null]");
    check({"null", "true", "null", R"("text")"}, R"([null,true,null,"text"])");
}

TEST(Ruvia, model_json_codec_roundtrips_default_dynamic_objects) {
    auto scalar = ruvia::json_value::parse("true");
    codec_dynamic value;
    value.set<"value">(std::move(scalar));
    value.set<"object">(ruvia::json_object{});
    const auto output = ruvia::to_json(value);
    EXPECT_EQ(std::string_view(output), R"({"value":true,"object":{}})");
    const auto reparsed = ruvia::from_json<codec_dynamic>(output);
    EXPECT_EQ(reparsed.get<"object">().view(), std::string_view("{}"));
    EXPECT_EQ(std::string_view(ruvia::to_json(reparsed)), std::string_view(output));
}

TEST(Ruvia, model_json_codec_owns_dynamic_tokens_and_preserves_retained_results) {
    {
        codec_dynamic retained;
        std::string retained_json;
        std::string input(R"({"value":{"keep":")");
        input.append(256, 'q');
        input += R"("},"object":{"n":1}})";
        auto parsed_value = ruvia::from_json<codec_dynamic>(input);
        retained = std::move(parsed_value);
        input.assign(input.size(), 'x');
        std::string expected_token(R"({"keep":")");
        expected_token.append(256, 'q');
        expected_token += R"("})";
        EXPECT_EQ(retained.get<"value">().view(), expected_token);
        retained_json = ruvia::to_json(retained);
        const auto saved = std::string(retained_json);
        const std::string temporary_input("{\"value\":\"" + std::string(160, 't') +
                                          "\",\"object\":{\"later\":true}}");
        for (std::size_t i = 0; i < 32; ++i) {
            {
                auto temporary = ruvia::from_json<codec_dynamic>(temporary_input);
                const auto output = ruvia::to_json(temporary);
                EXPECT_EQ(std::string_view(output), temporary_input);
            }
            EXPECT_EQ(retained.get<"value">().view(), expected_token);
            EXPECT_EQ(std::string_view(retained_json), saved);
        }
        const std::string invalid_input("{\"value\":\"" + std::string(160, 'i') + "\",\"object\":[]}");
        for (std::size_t i = 0; i < 32; ++i) {
            EXPECT_TRUE(ruvia::test::throws_on([&] {
                (void)ruvia::from_json<codec_dynamic>(invalid_input);
            }));
        }
    }
}

TEST(Ruvia, model_json_codec_roundtrips_empty_schema_and_rejects_non_object_root) {
    auto parsed_value = ruvia::from_json<codec_empty>(R"({})");
    const auto output = ruvia::to_json(parsed_value);
    EXPECT_EQ(std::string_view(output), "{}");
    auto reparsed = ruvia::from_json<codec_empty>(output);
    EXPECT_EQ(std::string_view(ruvia::to_json(reparsed)), "{}");
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<codec_empty>(R"([])"); }));
}

TEST(Ruvia, json_array_limits_apply_across_nested_collections_and_model_fields) {
    using nested_array = ruvia::array<ruvia::array<ruvia::int32>>;
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<nested_array>("[[1,2],[3]]", {.max_array_elements_ = 5});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<nested_array>("[[1,2],[3]]", {.max_array_elements_ = 4});
    }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<nested_array>("[]", {.max_array_elements_ = 0});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<nested_array>("[[]]", {.max_array_elements_ = 0});
    }));

    constexpr auto tree = R"({"name":"a","children":[{"name":"b","children":[{"name":"c"}]},{"name":"d"}]})";
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_node>(tree, {.max_array_elements_ = 3});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_node>(tree, {.max_array_elements_ = 2});
    }));
}

TEST(Ruvia, json_array_limits_include_dynamic_tokens_and_unknown_fields) {
    constexpr auto nested = R"({"value":[[1,2],[3]],"object":{}})";
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_dynamic>(nested, {.max_array_elements_ = 5});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_dynamic>(nested, {.max_array_elements_ = 4});
    }));

    constexpr auto dynamic = R"({"value":[1,2],"object":{"nested":[3]}})";
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_dynamic>(dynamic, {.max_array_elements_ = 3});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_dynamic>(dynamic, {.max_array_elements_ = 2});
    }));

    constexpr auto mixed = R"({"dense":[[1,2]],"boxed":[{"nested":[3]}]})";
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_dynamic_sequences>(mixed, {.max_array_elements_ = 5});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_dynamic_sequences>(mixed, {.max_array_elements_ = 4});
    }));

    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_empty>(R"({"unknown":[1,2]})", {.max_array_elements_ = 2});
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<codec_empty>(R"({"unknown":[1,2]})", {.max_array_elements_ = 1});
    }));
}

TEST(Ruvia, json_representation_budget_bounds_scalar_and_owned_string_results) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<ruvia::int32>("1", {.max_representation_bytes_ = 0});
    }));

    const std::string text("\"" + std::string(4096, 'x') + "\"");
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<ruvia::string>(text, {.max_representation_bytes_ = 1024});
    }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<ruvia::string>(text, {.max_representation_bytes_ = 16384});
    }));

    // A literal string is retained as one sized copy and is charged once;
    // decoding an escaped string grows its buffer, so it is charged twice.
    constexpr std::size_t single_copy_budget = 4096 + 1024;
    EXPECT_TRUE(!ruvia::test::throws_on([&] {
        (void)ruvia::from_json<ruvia::string>(text, {.max_representation_bytes_ = single_copy_budget});
    }));
    const std::string escaped("\"\\n" + std::string(4094, 'x') + "\"");
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<ruvia::string>(escaped, {.max_representation_bytes_ = single_copy_budget});
    }));
}

TEST(Ruvia, json_representation_budget_includes_decoded_field_names) {
    const std::string key(R"(\u0075)" + std::string(2048, 'x'));
    const std::string member(std::string("\"") + key + "\":0");
    const std::string single(std::string("{") + member + "}");
    const std::string repeated(std::string("{") + member + "," + member + "}");
    const auto one_key_budget = sizeof(codec_empty) + key.size() + 128;
    {
        auto parsed_value = ruvia::from_json<codec_empty>(single,
            {.max_representation_bytes_ = one_key_budget});
        EXPECT_EQ(std::string_view(ruvia::to_json(parsed_value)), "{}");
        EXPECT_TRUE(ruvia::test::throws_on([&] {
            (void)ruvia::from_json<codec_empty>(single,
                {.max_representation_bytes_ = sizeof(codec_empty) + 1024});
        }));
        EXPECT_TRUE(ruvia::test::throws_on([&] {
            (void)ruvia::from_json<codec_empty>(repeated,
                {.max_representation_bytes_ = one_key_budget});
        }));
    }
}

TEST(Ruvia, json_budget_failure_releases_partial_results_and_preserves_retained_values) {
    {
        auto retained = ruvia::from_json<ruvia::array<ruvia::string>>(
            R"(["retained value with owned storage"])");
        for (std::size_t index = 0; index < 8; ++index) {
            EXPECT_TRUE(ruvia::test::throws_on([&] {
                (void)ruvia::from_json<ruvia::boxed_array<ruvia::string>>(
                    R"(["first element with owned storage","second element with owned storage"])",
                    {.max_array_elements_ = 1});
            }));
            EXPECT_EQ(retained[0].view(), std::string_view("retained value with owned storage"));
        }
    }
}

TEST(Ruvia, model_json_codec_validates_field_rules_on_root_and_nested_model_arrays) {
    predicate_evaluations = 0;
    const auto valid_dense = ruvia::from_json<ruvia::array<codec_rules>>(R"([{"value":"valid"}])");
    EXPECT_EQ(valid_dense.size(), std::size_t{1});
    EXPECT_EQ(predicate_evaluations, std::size_t{1});

    const auto valid_boxed = ruvia::from_json<ruvia::boxed_array<codec_rules>>(R"([{"value":"valid"}])");
    EXPECT_EQ(valid_boxed.size(), std::size_t{1});
    EXPECT_EQ(predicate_evaluations, std::size_t{2});

    for (const bool boxed : {false, true}) {
        bool rejected = false;
        try {
            if (boxed) {
                (void)ruvia::from_json<ruvia::boxed_array<codec_rules>>(
                    R"([{"value":"valid"},{"value":"invalid"}])");
            } else {
                (void)ruvia::from_json<ruvia::array<codec_rules>>(
                    R"([{"value":"valid"},{"value":"invalid"}])");
            }
        } catch (const ruvia::http_error& error) {
            rejected = true;
            EXPECT_EQ(error.validation_issues().size(), std::size_t{1});
            if (error.validation_issues().size() == 1) {
                EXPECT_EQ(error.validation_issues()[0].field(), std::string_view("[1].value"));
                EXPECT_EQ(error.validation_issues()[0].code(), std::string_view("custom"));
                EXPECT_EQ(error.validation_issues()[0].message(), std::string_view("not valid"));
            }
        }
        EXPECT_TRUE(rejected);
    }

    bool nested_rejected = false;
    try {
        (void)ruvia::from_json<ruvia::array<ruvia::array<codec_rules>>>(
            R"([[{"value":"valid"}],[{"value":"bad"}]])");
    } catch (const ruvia::http_error& error) {
        nested_rejected = true;
        EXPECT_EQ(error.validation_issues().size(), std::size_t{1});
        if (error.validation_issues().size() == 1) {
            EXPECT_EQ(error.validation_issues()[0].field(), std::string_view("[1][0].value"));
            EXPECT_EQ(error.validation_issues()[0].code(), std::string_view("custom"));
        }
    }
    EXPECT_TRUE(nested_rejected);
}

namespace {

std::size_t evaluations{0};

int counted_default() {
    ++default_evaluations;
    return 11;
}

int initial_number() {
    ++evaluations;
    return 7;
}

std::string_view initial_name() {
    ++evaluations;
    return "seed";
}

int failing_initial() {
    ++evaluations;
    throw std::runtime_error("initial evaluation failed");
}

RUVIA_MODEL(initial_model,
    RUVIA_REQUIRED_FIELD(required, ruvia::int32),
    RUVIA_OPTIONAL_FIELD(number, ruvia::int32, RUVIA_INITIAL(initial_number()),
        RUVIA_DEFAULT(9)),
    RUVIA_OPTIONAL_FIELD(name, ruvia::string, RUVIA_INITIAL(initial_name())));

RUVIA_MODEL(failing_initial_model,
    RUVIA_OPTIONAL_FIELD(name, ruvia::string, RUVIA_INITIAL(std::string(128, 'n'))),
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_INITIAL(failing_initial())));

RUVIA_MODEL(required_initial_model,
    RUVIA_REQUIRED_FIELD(value, ruvia::int32, RUVIA_INITIAL(initial_number())));

RUVIA_MODEL(null_initial_model,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_NULLABLE, RUVIA_INITIAL(nullptr)));

RUVIA_MODEL(nested_initial_model,
    RUVIA_OPTIONAL_FIELD(child, initial_model),
    RUVIA_OPTIONAL_FIELD(children, ruvia::array<initial_model>));

RUVIA_MODEL(counted_lifecycle,
    RUVIA_OPTIONAL_FIELD(value, ruvia::int32, RUVIA_INITIAL(initial_number()),
        RUVIA_DEFAULT(counted_default())));

constexpr std::size_t initial_payload_size = 4096;
std::size_t payload_evaluations{0};

std::string initial_payload() {
    ++payload_evaluations;
    return std::string(initial_payload_size, 'p');
}

RUVIA_MODEL(payload_initial_model,
    RUVIA_OPTIONAL_FIELD(payload, ruvia::string, RUVIA_INITIAL(initial_payload())));

}  // namespace

TEST(Ruvia, model_initial_values_only_run_for_explicit_construction) {
    evaluations = 0;
    initial_model model;
    EXPECT_EQ(evaluations, std::size_t{2});
    EXPECT_EQ(model.get<"number">()->value_, 7);
    EXPECT_EQ(model.get<"name">()->view(), std::string_view("seed"));
    EXPECT_TRUE(!model.is_present<"number">());
    EXPECT_TRUE(!model.is_present<"name">());

    auto parsed_value = ruvia::from_json<initial_model>(R"({"required":1})");
    EXPECT_TRUE(parsed_value.get<"number">()->value_ == 9);
    EXPECT_TRUE(!parsed_value.get<"name">());
    EXPECT_TRUE(!parsed_value.is_present<"number">());
    EXPECT_TRUE(!parsed_value.is_null<"name">());
    EXPECT_EQ(evaluations, std::size_t{2});
}

TEST(Ruvia, model_initial_values_do_not_recur_through_moves_or_rebind) {
    evaluations = 0;
    initial_model retained;
    EXPECT_EQ(evaluations, std::size_t{2});
    auto parsed_value = ruvia::from_json<initial_model>(R"({"required":1,"number":3})");
    EXPECT_EQ(evaluations, std::size_t{2});
    retained = std::move(parsed_value);
    EXPECT_EQ(evaluations, std::size_t{2});
    EXPECT_EQ(retained.get<"number">()->value_, 3);
}

TEST(Ruvia, model_initial_exception_releases_partial_values) {
    evaluations = 0;
    bool failed = false;
    try {
        failing_initial_model model;
    } catch (const std::runtime_error& error) {
        failed = std::string_view(error.what()) == "initial evaluation failed";
    }
    EXPECT_TRUE(failed);
}

TEST(Ruvia, model_null_initial_is_a_value_state_without_allocation) {
    null_initial_model model;
    EXPECT_TRUE(model.is_null<"value">());
}

TEST(Ruvia, model_required_initial_is_not_used_by_parsing) {
    evaluations = 0;
    EXPECT_EQ(evaluations, std::size_t{0});
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_json<required_initial_model>("{}"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::from_form<required_initial_model>(""); }));
    EXPECT_EQ(evaluations, std::size_t{0});
}

TEST(Ruvia, model_initial_is_only_for_explicit_nested_construction) {
    evaluations = 0;
    nested_initial_model model;
    EXPECT_EQ(evaluations, std::size_t{0});
    (void)model.ensure<"child">();
    EXPECT_EQ(evaluations, std::size_t{2});
}

TEST(Ruvia, model_initial_does_not_recur_in_nested_move_or_array_emplace) {
    evaluations = 0;
    initial_model child_value;
    nested_initial_model parent_value;
    parent_value.set<"child">(std::move(child_value));
    EXPECT_EQ(evaluations, std::size_t{2});

    initial_model array_child;
    parent_value.ensure<"children">().emplace_back(std::move(array_child));
    EXPECT_EQ(evaluations, std::size_t{4});
    EXPECT_EQ(parent_value.get<"children">()->size(), std::size_t{1});
}

TEST(Ruvia, model_initial_and_default_modes_are_evaluated_once_across_codecs_and_moves) {
    evaluations = 0;
    default_evaluations = 0;
    counted_lifecycle explicit_value;
    EXPECT_EQ(evaluations, std::size_t{1});
    EXPECT_EQ(default_evaluations, std::size_t{0});

    auto json = ruvia::from_json<counted_lifecycle>("{}");
    auto form = ruvia::from_form<counted_lifecycle>("");
    EXPECT_EQ(evaluations, std::size_t{1});
    EXPECT_EQ(default_evaluations, std::size_t{2});
    counted_lifecycle moved(std::move(json));
    EXPECT_TRUE(!json.get<"value">());
    EXPECT_EQ(moved.get<"value">()->value_, 11);
    EXPECT_EQ(form.get<"value">()->value_, 11);
    EXPECT_TRUE(!moved.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(moved)), std::string_view(R"({"value":11})"));
    EXPECT_EQ(evaluations, std::size_t{1});
    EXPECT_EQ(default_evaluations, std::size_t{2});
}

TEST(Ruvia, model_initial_fresh_container_elements_are_built_once) {
    payload_evaluations = 0;
    ruvia::array<payload_initial_model> dense;
    ruvia::boxed_array<payload_initial_model> boxed;
    dense.reserve(4);
    boxed.reserve(4);
    for (std::size_t index = 0; index != 4; ++index) {
        (void)dense.emplace_back();
        (void)boxed.emplace_back();
    }
    EXPECT_EQ(payload_evaluations, std::size_t{8});
    dense.resize(6);
    boxed.resize(6);
    EXPECT_EQ(payload_evaluations, std::size_t{12});
    for (const auto& value : dense) {
        EXPECT_EQ(value.get<"payload">()->size(), initial_payload_size);
    }
    for (const auto& value : boxed) {
        EXPECT_EQ(value.get<"payload">()->size(), initial_payload_size);
    }
}

RUVIA_MODEL(json_object_child, RUVIA_REQUIRED_FIELD(name, ruvia::string));
RUVIA_MODEL(json_object_envelope,
    RUVIA_REQUIRED_FIELD(child, json_object_child),
    RUVIA_REQUIRED_FIELD(items, ruvia::array<json_object_child>));
RUVIA_MODEL(json_token_holder,
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object),
    RUVIA_OPTIONAL_FIELD(items, ruvia::array<ruvia::json_value>));

RUVIA_MODEL(json_validated_child,
    RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_MIN(3, "name too short")));

// get<T>() may borrow the token's bytes, so only lvalue tokens offer it.
template <typename token_type>
concept token_offers_get = requires(token_type&& token) {
    std::forward<token_type>(token).template get<ruvia::string>();
};
template <typename token_type>
concept token_offers_field_get = requires(token_type&& token) {
    std::forward<token_type>(token).template get<ruvia::string>("name");
};
static_assert(token_offers_get<const ruvia::json_value&>);
static_assert(!token_offers_get<ruvia::json_value>);
static_assert(!token_offers_get<const ruvia::json_value>);
static_assert(token_offers_field_get<const ruvia::json_value&>);
static_assert(!token_offers_field_get<ruvia::json_value>);
static_assert(token_offers_field_get<const ruvia::json_object&>);
static_assert(!token_offers_field_get<ruvia::json_object>);
static_assert(!token_offers_field_get<const ruvia::json_object>);

// A lookup hands its value out by reference only from an lvalue and by move
// from a mutable rvalue, so no reference into a temporary lookup escapes.
using string_lookup = ruvia::json_field_lookup<ruvia::string>;
template <typename lookup_type>
concept lookup_offers_value = requires(lookup_type&& lookup) { std::forward<lookup_type>(lookup).value(); };
static_assert(std::is_same_v<decltype(std::declval<const string_lookup&>().value()), const std::optional<ruvia::string>&>);
static_assert(std::is_same_v<decltype(std::declval<string_lookup&>().value()), std::optional<ruvia::string>&>);
static_assert(std::is_same_v<decltype(std::declval<string_lookup>().value()), std::optional<ruvia::string>>);
static_assert(!lookup_offers_value<const string_lookup>);

using lookup_status = ruvia::unique_lookup_status;

TEST(Ruvia, json_token_moves_preserve_complete_tokens_and_reset_sources) {
    const std::string input("{\"payload\":\"" + std::string(160, 'x') + "\"}");
    const auto check = [&]<typename token_type>(std::string_view empty) {
        {
            for (const bool owning : {false, true}) {
                auto source = token_type::parse(input);
                if (owning) {
                    token_type owned;
                    owned = std::move(source);
                    source = std::move(owned);
                }
                token_type moved(std::move(source));
                EXPECT_EQ(moved.view(), std::string_view(input));
                EXPECT_EQ(source.view(), empty);
                token_type target;
                target = std::move(moved);
                EXPECT_EQ(target.view(), std::string_view(input));
                EXPECT_EQ(moved.view(), empty);
                auto* const self = &target;
                target = std::move(*self);
                EXPECT_EQ(target.view(), std::string_view(input));
                source = std::move(target);
                EXPECT_EQ(source.view(), std::string_view(input));
                EXPECT_EQ(target.view(), empty);
            }
        }
    };
    check.template operator()<ruvia::json_value>("null");
    check.template operator()<ruvia::json_object>("{}");
}

TEST(Ruvia, json_token_field_transfers_leave_the_source_model_serializable) {
    auto source = ruvia::from_json<json_token_holder>(R"({"object":{"keep":true},"items":[[1,2]]})");
    ruvia::json_object object(std::move(source.ensure<"object">()));
    EXPECT_EQ(object.view(), std::string_view(R"({"keep":true})"));
    EXPECT_EQ(ruvia::to_json(source), R"({"object":{},"items":[[1,2]]})");
    json_token_holder target;
    target.set<"object">(std::move(object));
    EXPECT_EQ(object.view(), std::string_view("{}"));
    auto& source_items = source.ensure<"items">();
    target.ensure<"items">().push_back(std::move(source_items[0]));
    EXPECT_TRUE(source_items[0].is_null());
    EXPECT_EQ(source_items[0].view(), std::string_view("null"));
    EXPECT_EQ(ruvia::to_json(source), R"({"object":{},"items":[null]})");
    EXPECT_EQ(ruvia::to_json(target), R"({"object":{"keep":true},"items":[[1,2]]})");
    target.set<"object">(std::move(target.ensure<"object">()));
    EXPECT_EQ(ruvia::to_json(target), R"({"object":{"keep":true},"items":[[1,2]]})");
}

TEST(Ruvia, json_value_parse_classifies_kinds_and_rejects_trailing_input) {
    const auto object = ruvia::json_value::parse(R"({"name":"ada"})");
    EXPECT_TRUE(object.is_object());
    EXPECT_TRUE(!object.is_array());
    EXPECT_TRUE(!object.is_null());
    EXPECT_EQ(object.view(), std::string_view(R"({"name":"ada"})"));

    const auto array_value = ruvia::json_value::parse("[1,2]");
    EXPECT_TRUE(array_value.is_array());

    const auto null_value = ruvia::json_value::parse("null");
    EXPECT_TRUE(null_value.is_null());

    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_value::parse(R"({"name":"ada"} trailing)"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_value::parse("{"); }));
}

TEST(Ruvia, json_object_parse_requires_a_complete_object) {
    const auto json = ruvia::json_object::parse(R"({"name":"ada"})");
    EXPECT_EQ(json.view(), std::string_view(R"({"name":"ada"})"));

    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_object::parse("[1,2]"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_object::parse("null"); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_object::parse(R"({"name":"ada"} 1)"); }));
}

TEST(Ruvia, json_object_default_represents_an_empty_object) {
    const ruvia::json_object object;
    EXPECT_EQ(object.view(), std::string_view("{}"));
    const auto parsed = ruvia::json_object::parse(object.view());
    EXPECT_TRUE(object == parsed);
    EXPECT_TRUE(object.get<ruvia::string>("missing").status() == lookup_status::absent);
    std::size_t fields = 0;
    EXPECT_TRUE(object.for_each_field([&](std::string_view, const ruvia::json_value&) {
        ++fields;
        return true;
    }));
    EXPECT_EQ(fields, std::size_t{0});
}

TEST(Ruvia, json_object_get_reports_a_repeated_field_name_without_a_value) {
    const auto json = ruvia::json_object::parse(R"({"name":"first","other":"x","name":"second"})");
    const auto name = json.get<ruvia::string>("name");
    EXPECT_TRUE(name.status() == lookup_status::repeated);
    EXPECT_TRUE(!name.value().has_value());
    const auto token = json.get<ruvia::json_value>("name");
    EXPECT_TRUE(token.status() == lookup_status::repeated);
    EXPECT_TRUE(!token.value().has_value());
    const auto other = json.get<ruvia::string>("other");
    EXPECT_TRUE(other.status() == lookup_status::found);
    EXPECT_TRUE(other.value().has_value());
    EXPECT_EQ(other.value()->view(), std::string_view("x"));
    EXPECT_TRUE(json.get<ruvia::string>("missing").status() == lookup_status::absent);

    // Repetition is decided on decoded names and regardless of which
    // occurrence would convert to the requested type.
    const auto escaped = ruvia::json_object::parse(R"({"na\u006de":"first","name":"second"})");
    EXPECT_TRUE(escaped.get<ruvia::string>("name").status() == lookup_status::repeated);
    const auto mixed = ruvia::json_object::parse(R"({"count":"text","count":2})");
    const auto count = mixed.get<ruvia::int32>("count");
    EXPECT_TRUE(count.status() == lookup_status::repeated);
    EXPECT_TRUE(!count.value().has_value());
    const auto children = ruvia::json_object::parse(R"({"child":{"name":"ada"},"child":{"name":"bob"}})");
    EXPECT_TRUE(children.get<json_object_child>("child").status() == lookup_status::repeated);
    // A third occurrence does not change the answer.
    const auto triple = ruvia::json_object::parse(R"({"k":1,"k":2,"k":3})");
    EXPECT_TRUE(triple.get<ruvia::int32>("k").status() == lookup_status::repeated);

    const auto value = ruvia::json_value::parse(R"({"name":"first","name":"second","id":7})");
    EXPECT_TRUE(value.get<ruvia::string>("name").status() == lookup_status::repeated);
    const auto id = value.get<ruvia::int32>("id").value();
    EXPECT_TRUE(id.has_value());
    EXPECT_EQ(static_cast<std::int32_t>(*id), 7);
}

TEST(Ruvia, json_value_dynamic_traversal_decodes_keys_and_preserves_order) {
    auto json = ruvia::json_value::parse(R"({"a\u0062":1,"a\u0062":2,"nested":[3,4]})");
    int count = 0;
    EXPECT_TRUE(json.for_each_field([&](std::string_view key, const ruvia::json_value& value) {
        ++count;
        if (count < 3) {
            EXPECT_EQ(key, std::string_view("ab"));
            EXPECT_TRUE(value.is_number());
        } else {
            EXPECT_EQ(key, std::string_view("nested"));
            EXPECT_TRUE(value.is_array());
        }
        return true;
    }));
    EXPECT_EQ(count, 3);
}

TEST(Ruvia, json_value_dynamic_traversal_handles_whitespace_and_early_stop) {
    auto array_value = ruvia::json_value::parse("[1, \n 2,\t3]");
    int seen = 0;
    EXPECT_TRUE(array_value.for_each_element([&](const ruvia::json_value& value) {
        ++seen;
        EXPECT_TRUE(value.is_number());
        return true;
    }));
    EXPECT_EQ(seen, 3);

    auto object = ruvia::json_value::parse("{\"plain\": 1, \n \"escaped\\u03bb\": 2}");
    EXPECT_TRUE(object.for_each_field([](std::string_view key, const ruvia::json_value&) {
        return key == "plain" || key == "escapedλ";
    }));

    int stopped = 0;
    EXPECT_TRUE(!array_value.for_each_element([&](const ruvia::json_value&) {
        ++stopped;
        return false;
    }));
    EXPECT_EQ(stopped, 1);
}

TEST(Ruvia, json_value_dynamic_traversal_handles_empty_wrong_kind_and_early_stop) {
    auto empty = ruvia::json_value::parse("[]");
    auto object = ruvia::json_value::parse("{}");
    auto scalar = ruvia::json_value::parse("1");
    EXPECT_TRUE(empty.for_each_element([](const ruvia::json_value&) { return true; }));
    EXPECT_TRUE(object.for_each_field([](std::string_view, const ruvia::json_value&) { return true; }));
    EXPECT_TRUE(!scalar.for_each_element([](const ruvia::json_value&) { return true; }));

    auto array_value = ruvia::json_value::parse("[1,2,3]");
    int seen = 0;
    EXPECT_TRUE(!array_value.for_each_element([&](const ruvia::json_value&) {
        ++seen;
        return false;
    }));
    EXPECT_EQ(seen, 1);
}

TEST(Ruvia, json_value_object_traversal_reports_completion_and_stops_once) {
    const auto empty = ruvia::json_value::parse("{}");
    const auto object = ruvia::json_value::parse(R"({"a":1,"b":2,"c":3})");
    int calls = 0;
    const auto stop = [&](std::string_view, const ruvia::json_value&) {
        ++calls;
        return false;
    };
    EXPECT_TRUE(empty.for_each_field(stop));
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(!object.for_each_field(stop));
    EXPECT_EQ(calls, 1);
    calls = 0;
    EXPECT_TRUE(!object.for_each_field([&](std::string_view, const ruvia::json_value&) {
        return ++calls < 2;
    }));
    EXPECT_EQ(calls, 2);
    calls = 0;
    EXPECT_TRUE(object.for_each_field([&](std::string_view, const ruvia::json_value&) {
        ++calls;
        return true;
    }));
    EXPECT_EQ(calls, 3);
}

TEST(Ruvia, json_value_dynamic_traversal_propagates_callback_exceptions_and_compares_tokens) {
    auto first = ruvia::json_value::parse("[1]");
    auto second = ruvia::json_value::parse("[1]");
    auto different = ruvia::json_value::parse("[1.0]");
    EXPECT_TRUE(first == second);
    EXPECT_TRUE(!(first == different));
    bool propagated = false;
    try {
        (void)first.for_each_element([](const ruvia::json_value&) -> bool {
            throw std::runtime_error("stop");
        });
    } catch (const std::runtime_error&) {
        propagated = true;
    }
    EXPECT_TRUE(propagated);
}

TEST(Ruvia, json_value_get_reads_root_scalars_and_arrays_and_models) {
    auto string_value = ruvia::json_value::parse("\"hello\"");
    auto boolean = ruvia::json_value::parse("true");
    EXPECT_EQ(string_value.get<ruvia::string>()->view(), std::string_view("hello"));
    EXPECT_TRUE(*boolean.get<ruvia::bool_value>());

    auto valid_model = ruvia::json_value::parse(R"({"child":{"name":"ada"},"items":[{"name":"grace"}]})");
    EXPECT_TRUE(valid_model.get<json_object_envelope>().has_value());
    auto missing_required = ruvia::json_value::parse(R"({"child":{}})");
    EXPECT_TRUE(!missing_required.get<json_object_envelope>().has_value());
    auto invalid_nested_array = ruvia::json_value::parse(R"({"child":{"name":"ada"},"items":[{}]})");
    EXPECT_TRUE(!invalid_nested_array.get<json_object_envelope>().has_value());

    auto number = ruvia::json_value::parse("9007199254740993");
    const auto value = number.get<ruvia::uint64>();
    EXPECT_TRUE(value.has_value());
    EXPECT_EQ(static_cast<std::uint64_t>(*value), 9007199254740993ULL);

    auto array_value = ruvia::json_value::parse("[1,2]");
    const auto parsed_value = array_value.get<ruvia::array<ruvia::int32>>();
    EXPECT_TRUE(parsed_value.has_value());
    EXPECT_EQ(parsed_value->size(), 2U);
    EXPECT_TRUE(!number.get<ruvia::array<ruvia::int32>>().has_value());
    auto valid_rule = ruvia::json_value::parse(R"({"name":"ada"})");
    EXPECT_TRUE(valid_rule.get<json_validated_child>().has_value());
    auto invalid_rule = ruvia::json_value::parse(R"({"name":"ab"})");
    EXPECT_TRUE(!invalid_rule.get<json_validated_child>().has_value());

    auto obj_rule = ruvia::json_object::parse(R"({"good":{"name":"ada"},"bad":{"name":"ab"}})");
    const auto good = obj_rule.get<json_validated_child>("good");
    EXPECT_TRUE(good.status() == lookup_status::found);
    EXPECT_TRUE(good.value().has_value());
    // Present but failing the model's rules: found without a value.
    const auto bad = obj_rule.get<json_validated_child>("bad");
    EXPECT_TRUE(bad.status() == lookup_status::found);
    EXPECT_TRUE(!bad.value().has_value());
}
TEST(Ruvia, json_value_dynamic_traversal_reclaims_decoded_key_on_exception) {
    std::string body("{\"" + std::string(256, 'k') + "\\u0061\":1}");
    const auto json = ruvia::json_value::parse(body);
    bool thrown = false;
    try {
        // The throw depends on the visited key, as in a real visitor: one that
        // can never return leaves the traversal after it unreachable, which
        // optimizing compilers report (MSVC C4702).
        (void)json.for_each_field([](std::string_view key, const ruvia::json_value&) {
            if (key.ends_with('a')) {
                throw std::runtime_error("visitor");
            }
            return true;
        });
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    EXPECT_TRUE(thrown);
}

TEST(Ruvia, json_value_get_reads_object_fields_and_rejects_non_objects) {
    auto json =
        ruvia::json_value::parse(R"({"name":"ada","age":2})");
    const auto age = json.get<ruvia::int32>("age");
    EXPECT_TRUE(age.status() == lookup_status::found);
    EXPECT_TRUE(age.value().has_value());
    EXPECT_EQ(static_cast<std::int32_t>(*age.value()), 2);
    const auto mismatched = json.get<ruvia::int32>("name");
    EXPECT_TRUE(mismatched.status() == lookup_status::found);
    EXPECT_TRUE(!mismatched.value().has_value());
    const auto missing = json.get<ruvia::string>("missing");
    EXPECT_TRUE(missing.status() == lookup_status::absent);
    EXPECT_TRUE(!missing.value().has_value());

    // A non-object token has no members.
    auto array_value = ruvia::json_value::parse("[1]");
    EXPECT_TRUE(array_value.get<ruvia::int32>("age").status() == lookup_status::absent);
}

TEST(Ruvia, json_dynamic_parse_enforces_cumulative_array_element_budget) {
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_value::parse("[1]", {.max_array_elements_ = 0}); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_value::parse("[]", {.max_array_elements_ = 0}); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_value::parse("[1]", {.max_array_elements_ = 1}); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_value::parse("[[1],[2]]", {.max_array_elements_ = 3}); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_value::parse("[[1],[2]]", {.max_array_elements_ = 4}); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_value::parse(R"({"a":{"b":[1,2]}})", {.max_array_elements_ = 1}); }));

    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_object::parse(R"({"a":[1]})", {.max_array_elements_ = 0}); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_object::parse(R"({"a":[1]})", {.max_array_elements_ = 1}); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::json_object::parse(R"({"a":{"b":[1,2]},"c":[3]})", {.max_array_elements_ = 2});
    }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_object::parse(" {} ", {.max_array_elements_ = 0}); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_object::parse("[]"); }));

    const auto elements = [](std::size_t count) {
        std::string body(R"({"a":[)");
        for (std::size_t index = 0; index != count; ++index) {
            body.append(index == 0 ? "0" : ",0");
        }
        body.append("]}");
        return body;
    };
    const auto at_default_limit = elements(64 * 1024);
    const auto over_default_limit = elements(64 * 1024 + 1);
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_value::parse(at_default_limit); }));
    EXPECT_TRUE(!ruvia::test::throws_on([&] { (void)ruvia::json_object::parse(at_default_limit); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_value::parse(over_default_limit); }));
    EXPECT_TRUE(ruvia::test::throws_on([&] { (void)ruvia::json_object::parse(over_default_limit); }));
}

using namespace ruvia::test;

TEST(Ruvia, model_json_parser_dispatches_decoded_keys) {
    const auto parsed_value = ruvia::from_json<accessor_surface_request>(
        R"({"mess\u0061ge":"ready"})");
    EXPECT_TRUE(parsed_value.get<"message">().has_value());
    if (!parsed_value.get<"message">()) {
        return;
    }
    EXPECT_EQ(parsed_value.get<"message">()->view(), std::string_view("ready"));
}

TEST(Ruvia, model_public_parsers_own_string_fields) {
    auto json = ruvia::from_json<accessor_surface_request>(
        std::string(R"({"message":"json-owned"})"));
    std::string form_input("message=form-owned");
    auto form = ruvia::from_form<accessor_surface_request>(form_input);
    form_input.assign(form_input.size(), 'x');

    if (!json.get<"message">() || !form.get<"message">()) {
        return;
    }
    EXPECT_EQ(json.get<"message">()->view(), std::string_view("json-owned"));
    EXPECT_EQ(form.get<"message">()->view(), std::string_view("form-owned"));
}

TEST(Ruvia, model_form_parser_keeps_malformed_percent_sequences_literally) {
    for (const std::string_view invalid : {"%", "%G0", "%0G"}) {
        for (const auto& body : {
                 std::move(std::string("message=ready&unknown=").append(invalid)),
                 std::move(std::string("unknown=").append(invalid)) + "&message=ready",
                 std::move(std::string("message=ready&").append(invalid)) + "=ignored"}) {
            const auto parsed = ruvia::from_form<accessor_surface_request>(body);
            EXPECT_TRUE(parsed.get<"message">().has_value());
        }
    }
    const auto literal = ruvia::from_form<accessor_surface_request>("message=100%");
    EXPECT_TRUE(literal.get<"message">().has_value());
    if (literal.get<"message">()) {
        EXPECT_EQ(literal.get<"message">()->view(), std::string_view("100%"));
    }
    const auto valid = ruvia::from_form<accessor_surface_request>(
        "message=hello+world&unknown=%00%26%3D%FF&%75nknown=value");
    EXPECT_TRUE(valid.get<"message">().has_value());
    if (valid.get<"message">()) {
        EXPECT_EQ(valid.get<"message">()->view(), std::string_view("hello world"));
    }
}

TEST(Ruvia, model_json_parser_rejects_invalid_field_types) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<nested_model_item>(
            R"({"id":"wrong","label":"still parsed"})");
    }));
}

TEST(Ruvia, model_json_parser_fully_validates_unknown_and_duplicate_values) {
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"ready","unknown":[1,]})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"ready","unknown":"\ud83d"})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"first","message":{"broken":}})");
    }));
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"first","message":"\ud83d"})");
    }));

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"first","message":"second"})");
    }));
}

TEST(Ruvia, model_json_parser_rejects_nested_structure_recursively) {
    constexpr auto body = R"({"primary":{},"items":[{"label":"missing id"}]})";
    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<nested_model_envelope>(body);
    }));
}

TEST(Ruvia, model_json_parser_enforces_depth_while_skipping_unknown_values) {
    std::string input(R"({"message":"ready","unknown":)");
    input.append(70, '[');
    input.append(70, ']');
    input.push_back('}');

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(input);
    }));
}

using namespace ruvia::test;

TEST(Ruvia, model_public_parser_accepts_valid_strings_and_rejects_invalid_input) {
    const auto model = ruvia::from_json<accessor_surface_request>(
        R"({"message":"ready"})");
    EXPECT_TRUE(model.get<"message">().has_value());
    if (model.get<"message">().has_value()) {
        EXPECT_EQ(model.get<"message">()->view(), std::string_view("ready"));
    }

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":42})");
    }));

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"incomplete")");
    }));
}

TEST(Ruvia, request_and_response_models_support_nested_arrays_and_optional_fields) {
    std::string input(R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})");
    const auto parsed_value = ruvia::from_json<nested_model_envelope>(input);
    input.assign(input.size(), 'x');

    EXPECT_EQ(std::uint32_t(parsed_value.get<"primary">().get<"id">()), std::uint32_t{1});
    EXPECT_EQ(parsed_value.get<"items">().size(), std::size_t{1});
    EXPECT_TRUE(parsed_value.get<"tags">().has_value());
    EXPECT_TRUE(!parsed_value.get<"primary">().get<"label">().has_value());
    EXPECT_TRUE(parsed_value.get<"items">()[0].get<"label">().has_value());
    if (parsed_value.get<"items">()[0].get<"label">()) {
        EXPECT_EQ(parsed_value.get<"items">()[0].get<"label">()->view(), std::string_view("two"));
    }
    if (parsed_value.get<"tags">()) {
        EXPECT_EQ(parsed_value.get<"tags">()->size(), std::size_t{2});
        EXPECT_EQ((*parsed_value.get<"tags">())[0].view(), std::string_view("a"));
        EXPECT_EQ((*parsed_value.get<"tags">())[1].view(), std::string_view("b"));
    }

    nested_response_envelope response;
    response.ensure<"primary">().set<"id">(ruvia::uint32{1});
    response.ensure<"items">()
        .emplace_back()
        .set<"id">(ruvia::uint32{2})
        .set<"label">("two");
    response.ensure<"tags">().emplace_back("a");
    response.ensure<"tags">().emplace_back("b");
    EXPECT_EQ(std::string_view(ruvia::to_json(response)),
        std::string_view(R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})"));
}

RUVIA_MODEL(request_binding_params,
    RUVIA_REQUIRED_FIELD(id, ruvia::string, RUVIA_MIN(2, "id is too short")));

RUVIA_MODEL(request_binding_body,
    RUVIA_REQUIRED_FIELD(name, ruvia::string, RUVIA_MIN(2, "name is too short")));

RUVIA_MODEL(patch_binding_body,
    RUVIA_REQUIRED_FIELD(token, ruvia::string),
    RUVIA_OPTIONAL_FIELD(retries, ruvia::uint32, RUVIA_DEFAULT(0), RUVIA_MIN(1, "must be positive")),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::string, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback"), RUVIA_MIN(2, "too short")),
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::bool_value));

RUVIA_MODEL(request_binding_out, RUVIA_REQUIRED_FIELD(id, ruvia::string),
    RUVIA_REQUIRED_FIELD(name, ruvia::string));

RUVIA_MODEL(dynamic_binding_body,
    RUVIA_OPTIONAL_FIELD(payload, ruvia::json_value),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object),
    RUVIA_OPTIONAL_FIELD(numbers, ruvia::array<ruvia::uint32>));

RUVIA_MODEL(tags_binding_input, RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>));

namespace {

ruvia::task<request_binding_out> make_response(std::string_view id, std::string_view name) {
    request_binding_out response;
    response.set<"id">(id);
    response.set<"name">(name);
    co_return response;
}

}  // namespace

class request_binding_controller final {
public:
    RUVIA_ROUTES_BEGIN(request_binding_controller)
    RUVIA_PUT("/request-binding/:id", update, ruvia::path_model<request_binding_params>,
        ruvia::json_body<request_binding_body>);
    RUVIA_PATCH("/request-binding-patch", patch, ruvia::json_body<patch_binding_body>);
    RUVIA_POST("/request-binding-echo", echo, ruvia::json_body<request_binding_body>);
    RUVIA_POST("/request-binding-dynamic", dynamic, ruvia::json_body<dynamic_binding_body>);
    RUVIA_POST("/request-binding-form", echo, ruvia::form_body<request_binding_body>);
    RUVIA_GET("/request-binding-query-tags", tags, ruvia::query_model<tags_binding_input>);
    RUVIA_GET("/request-binding-header-tags", tags, ruvia::header_model<tags_binding_input>);
    RUVIA_ROUTES_END

    ruvia::task<ruvia::http_response> tags(ruvia::context& c) {
        const bool present = c.req().validated<tags_binding_input>().is_present<"tags">();
        co_return c.text(std::string_view(present ? "present" : "absent"));
    }

    ruvia::task<ruvia::http_response> dynamic(ruvia::context& c) {
        co_return c.json(c.req().validated<dynamic_binding_body>());
    }

    ruvia::task<ruvia::http_response> echo(ruvia::context& c) {
        co_return c.json(c.req().validated<request_binding_body>());
    }

    ruvia::task<ruvia::http_response> patch(ruvia::context& c) {
        const auto& body = c.req().validated<patch_binding_body>();
        if (!body.is_present<"remark">()) {
            co_return c.text("unchanged");
        }
        if (body.is_null<"remark">()) {
            co_return c.text("clear");
        }
        co_return c.text(body.get<"remark">()->view());
    }

    ruvia::task<ruvia::http_response> update(ruvia::context& c) {
        const auto& params = c.req().validated<request_binding_params>();
        const auto& body = c.req().validated<request_binding_body>();
        auto response = co_await make_response(params.get<"id">().view(), body.get<"name">().view());
        co_return c.json(response);
    }
};

TEST(Ruvia, request_binding_enforces_defaults_and_exposes_patch_states) {
    ruvia::test_app app;
    for (const auto& [body, expected] : {
             std::pair{R"({"token":"ok","retries":1})", "unchanged"},
             std::pair{R"({"token":"ok","retries":1,"remark":null})", "clear"},
             std::pair{R"({"token":"ok","retries":1,"remark":"changed"})", "changed"}}) {
        const auto response = app.request(ruvia::test_request::patch("/request-binding-patch").json(body));
        EXPECT_TRUE(response.status() == ruvia::http_status::ok);
        EXPECT_EQ(response.body(), std::string_view(expected));
    }
    for (const auto& [body, code] : {
             std::pair{R"({"token":"ok"})", "too_small"},
             std::pair{R"({"retries":1})", "required"},
             std::pair{R"({"token":"ok","retries":1,"enabled":null})", "invalid_type"},
             std::pair{R"({"token":"ok","retries":1,"remark":""})", "too_small"},
             std::pair{R"({"token":"ok","retries":1,"remark":null,"remark":"changed"})", "duplicate"}}) {
        const auto response = app.request(ruvia::test_request::patch("/request-binding-patch").json(body));
        EXPECT_TRUE(response.status() == ruvia::http_status::bad_request);
        EXPECT_TRUE(response.body().find(code) != std::string_view::npos);
    }
}

TEST(Ruvia, request_binding_serializes_the_validated_model_without_copying_fields) {
    ruvia::test_app app;
    const auto ok = app.request(
        ruvia::test_request::post("/request-binding-echo").json(R"({"name":"A\u006c"})"));
    EXPECT_TRUE(ok.status() == ruvia::http_status::ok);
    EXPECT_EQ(ok.body(), std::string_view(R"({"name":"Al"})"));
    for (const auto body : {R"({"name":"A"})", R"({"name":null})", R"({})", R"({"name":"Al","name":"Bob"})"}) {
        const auto rejected = app.request(ruvia::test_request::post("/request-binding-echo").json(body));
        EXPECT_TRUE(rejected.status() == ruvia::http_status::bad_request);
    }
}

TEST(Ruvia, request_binding_form_keeps_malformed_percent_sequences_literally) {
    // WHATWG application/x-www-form-urlencoded parsing keeps a '%' that does not
    // start a valid escape.
    ruvia::test_app app;
    for (const std::string_view invalid : {"%", "%G0", "%0G"}) {
        for (const auto& body : {
                 std::move(std::string("name=Al").append(invalid)) + "&unknown=" + std::string(invalid),
                 std::move(std::string("unknown=").append(invalid)) + "&name=Al" + std::string(invalid)}) {
            const auto response = app.request(
                ruvia::test_request::post("/request-binding-form").form(body));
            EXPECT_TRUE(response.status() == ruvia::http_status::ok);
            EXPECT_EQ(response.body(), std::move(std::string(R"({"name":"Al)").append(invalid)) + R"("})");
        }
    }
    const auto valid = app.request(ruvia::test_request::post("/request-binding-form")
            .form("name=A%6C&unknown=%00%26%3D%FF"));
    EXPECT_TRUE(valid.status() == ruvia::http_status::ok);
    EXPECT_EQ(valid.body(), std::string_view(R"({"name":"Al"})"));
}

TEST(Ruvia, request_binding_validates_inputs_and_serializes_service_model) {
    ruvia::test_app app;
    const auto ok = app.request(
        ruvia::test_request::put("/request-binding/u1").json(R"({"name":"Al"})"));
    EXPECT_TRUE(ok.status() == ruvia::http_status::ok);
    EXPECT_TRUE(ok.body().find("\"id\":\"u1\"") != std::string_view::npos);
    EXPECT_TRUE(ok.body().find("\"name\":\"Al\"") != std::string_view::npos);

    const auto short_id = app.request(
        ruvia::test_request::put("/request-binding/x").json(R"({"name":"Al"})"));
    EXPECT_TRUE(short_id.status() == ruvia::http_status::bad_request);

    const auto short_name = app.request(
        ruvia::test_request::put("/request-binding/u1").json(R"({"name":"A"})"));
    EXPECT_TRUE(short_name.status() == ruvia::http_status::bad_request);
}

TEST(Ruvia, request_binding_dynamic_json_distinguishes_syntax_from_field_type_errors) {
    ruvia::test_app app;
    for (const auto body : {R"({"object":[1]})", R"({"object": [1]})", R"({"object":"x"})", R"({"object": "x"})"}) {
        const auto response = app.request(ruvia::test_request::post("/request-binding-dynamic").json(body));
        EXPECT_TRUE(response.status() == ruvia::http_status::bad_request);
        EXPECT_TRUE(response.body().find("invalid_type") != std::string_view::npos);
    }
    for (const auto body : {R"({"payload":})", R"({"payload": })", R"({"payload":[1,})", R"({"object":{"x":}})"}) {
        const auto response = app.request(ruvia::test_request::post("/request-binding-dynamic").json(body));
        EXPECT_TRUE(response.status() == ruvia::http_status::bad_request);
        EXPECT_TRUE(response.body().find("invalid json body") != std::string_view::npos);
        EXPECT_TRUE(response.body().find("validation_failed") == std::string_view::npos);
    }
}

TEST(Ruvia, request_binding_large_wrong_type_arrays_report_field_validation) {
    std::string elements{};
    for (std::size_t index = 0; index < 40000; ++index) {
        if (index != 0) {
            elements.push_back(',');
        }
        elements.push_back('0');
    }
    ruvia::test_app app;
    for (const auto& body : {
             std::string("{\"object\":[") + elements + "]}",
             std::string("{\"numbers\":[") + elements + ",\"wrong type\"]}"}) {
        const auto response = app.request(ruvia::test_request::post("/request-binding-dynamic").json(body));
        EXPECT_TRUE(response.status() == ruvia::http_status::bad_request);
        EXPECT_TRUE(response.body().find("validation_failed") != std::string_view::npos);
        EXPECT_TRUE(response.body().find("invalid_type") != std::string_view::npos);
        EXPECT_TRUE(response.body().find("invalid json body") == std::string_view::npos);
    }
}

TEST(Ruvia, request_binding_rejects_query_and_header_values_for_array_fields) {
    ruvia::test_app app;
    for (const auto target : {"/request-binding-query-tags", "/request-binding-header-tags"}) {
        const auto absent = app.request(ruvia::test_request::get(target));
        EXPECT_TRUE(absent.status() == ruvia::http_status::ok);
        EXPECT_EQ(absent.body(), std::string_view("absent"));
    }
    for (const auto& [target, code] : {
             std::pair{"/request-binding-query-tags?tags=a", "invalid_type"},
             std::pair{"/request-binding-query-tags?tags=a&tags=b", "duplicate"}}) {
        const auto response = app.request(ruvia::test_request::get(target));
        EXPECT_TRUE(response.status() == ruvia::http_status::bad_request);
        EXPECT_TRUE(response.body().find(code) != std::string_view::npos);
    }
    const auto header = app.request(ruvia::test_request::get("/request-binding-header-tags").header("Tags", "a"));
    EXPECT_TRUE(header.status() == ruvia::http_status::bad_request);
    EXPECT_TRUE(header.body().find("invalid_type") != std::string_view::npos);
}
