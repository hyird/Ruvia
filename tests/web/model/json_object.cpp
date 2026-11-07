#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/model.h"

#include "memory_resource_fixture.h"
#include "test_exceptions.h"

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
    const std::string input = "{\"payload\":\"" + std::string(160, 'x') + "\"}";
    const auto check = [&]<typename token_type>(std::string_view empty) {
        ruvia::test::counting_memory_resource source_resource;
        ruvia::test::counting_memory_resource target_resource;
        {
            for (const bool owning : {false, true}) {
                auto source = token_type::parse(input, {.resource_ = &source_resource});
                if (owning) {
                    token_type owned({.resource_ = &source_resource});
                    owned = std::move(source);
                    source = std::move(owned);
                }
                token_type moved(std::move(source));
                EXPECT_EQ(moved.view(), std::string_view(input));
                EXPECT_EQ(source.view(), empty);
                EXPECT_TRUE(source.resource() == &source_resource);
                token_type target({.resource_ = &target_resource});
                target = std::move(moved);
                EXPECT_EQ(target.view(), std::string_view(input));
                EXPECT_EQ(moved.view(), empty);
                EXPECT_TRUE(target.resource() == &target_resource);
                auto* const self = &target;
                target = std::move(*self);
                EXPECT_EQ(target.view(), std::string_view(input));
                source = std::move(target);
                EXPECT_EQ(source.view(), std::string_view(input));
                EXPECT_EQ(target.view(), empty);
            }
        }
        EXPECT_EQ(source_resource.live_allocations(), std::size_t{0});
        EXPECT_EQ(target_resource.live_allocations(), std::size_t{0});
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

    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse(R"({"name":"ada"} trailing)"); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse("{"); }));
}

TEST(Ruvia, json_object_parse_requires_a_complete_object) {
    const auto json = ruvia::json_object::parse(R"({"name":"ada"})");
    EXPECT_EQ(json.view(), std::string_view(R"({"name":"ada"})"));

    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse("[1,2]"); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse("null"); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse(R"({"name":"ada"} 1)"); }));
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
    const auto json = ruvia::json_object::parse(R"({"name":"first","other":"x","name":"second"})",
        {.resource_ = std::pmr::get_default_resource()});
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
    ruvia::test::counting_memory_resource resource;
    std::string body = "{\"" + std::string(256, 'k') + "\\u0061\":1}";
    const auto json = ruvia::json_value::parse(body, {.resource_ = &resource});
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
    EXPECT_EQ(resource.live_allocations(), 0U);
}

TEST(Ruvia, json_value_get_reads_object_fields_and_rejects_non_objects) {
    auto json =
        ruvia::json_value::parse(R"({"name":"ada","age":2})", {.resource_ = std::pmr::get_default_resource()});
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
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse("[1]", {.max_array_elements_ = 0}); }));
    EXPECT_TRUE(!ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse("[]", {.max_array_elements_ = 0}); }));
    EXPECT_TRUE(!ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse("[1]", {.max_array_elements_ = 1}); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse("[[1],[2]]", {.max_array_elements_ = 3}); }));
    EXPECT_TRUE(!ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse("[[1],[2]]", {.max_array_elements_ = 4}); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse(R"({"a":{"b":[1,2]}})", {.max_array_elements_ = 1}); }));

    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse(R"({"a":[1]})", {.max_array_elements_ = 0}); }));
    EXPECT_TRUE(!ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse(R"({"a":[1]})", {.max_array_elements_ = 1}); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::json_object::parse(R"({"a":{"b":[1,2]},"c":[3]})", {.max_array_elements_ = 2});
    }));
    EXPECT_TRUE(!ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse(" {} ", {.max_array_elements_ = 0}); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse("[]"); }));

    const auto elements = [](std::size_t count) {
        std::string body = R"({"a":[)";
        for (std::size_t index = 0; index != count; ++index) {
            body.append(index == 0 ? "0" : ",0");
        }
        body.append("]}");
        return body;
    };
    const auto at_default_limit = elements(64 * 1024);
    const auto over_default_limit = elements(64 * 1024 + 1);
    EXPECT_TRUE(!ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse(at_default_limit); }));
    EXPECT_TRUE(!ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse(at_default_limit); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse(over_default_limit); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse(over_default_limit); }));
}
