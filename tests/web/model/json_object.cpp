#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/model.h"
#include "ruvia/web/model_object.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

RUVIA_MODEL(json_object_child, RUVIA_REQUIRED_FIELD(name, ruvia::string));
RUVIA_MODEL(json_object_envelope,
    RUVIA_REQUIRED_FIELD(child, json_object_child),
    RUVIA_REQUIRED_FIELD(items, ruvia::array<json_object_child>));

RUVIA_TEST(json_value_parse_classifies_kinds_and_rejects_trailing_input) {
    const auto object = ruvia::json_value::parse(R"({"name":"ada"})");
    RUVIA_CHECK(object.is_object());
    RUVIA_CHECK(!object.is_array());
    RUVIA_CHECK(!object.is_null());
    RUVIA_CHECK_EQ(object.view(), std::string_view(R"({"name":"ada"})"));

    const auto array_value = ruvia::json_value::parse("[1,2]");
    RUVIA_CHECK(array_value.is_array());

    const auto null_value = ruvia::json_value::parse("null");
    RUVIA_CHECK(null_value.is_null());

    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse(R"({"name":"ada"} trailing)"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse("{"); }));
}

RUVIA_TEST(json_object_parse_requires_a_complete_object) {
    const auto json = ruvia::json_object::parse(R"({"name":"ada"})");
    RUVIA_CHECK_EQ(json.view(), std::string_view(R"({"name":"ada"})"));

    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse("[1,2]"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse("null"); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse(R"({"name":"ada"} 1)"); }));
}

RUVIA_TEST(json_object_default_represents_an_empty_object) {
    const ruvia::json_object object;
    RUVIA_CHECK_EQ(object.view(), std::string_view("{}"));
    const auto parsed = ruvia::json_object::parse(object.view());
    RUVIA_CHECK(object == parsed);
    RUVIA_CHECK(!object.get<ruvia::string>("missing"));
    std::size_t fields = 0;
    RUVIA_CHECK(object.for_each_field([&](std::string_view, const ruvia::json_value&) {
        ++fields;
        return true;
    }));
    RUVIA_CHECK_EQ(fields, std::size_t{0});
}

RUVIA_TEST(json_object_get_uses_last_match) {
    auto json = ruvia::json_object::parse(R"({"name":"first","other":"x","name":"second"})",
        {.resource_ = std::pmr::get_default_resource()});

    const auto value = json.get<ruvia::string>("name");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(value->view(), std::string_view("second"));
}

RUVIA_TEST(json_value_dynamic_traversal_decodes_keys_and_preserves_order) {
    auto json = ruvia::json_value::parse(R"({"a\u0062":1,"a\u0062":2,"nested":[3,4]})");
    int count = 0;
    RUVIA_CHECK(json.for_each_field([&](std::string_view key, const ruvia::json_value& value) {
        ++count;
        if (count < 3) {
            RUVIA_CHECK_EQ(key, std::string_view("ab"));
            RUVIA_CHECK(value.is_number());
        } else {
            RUVIA_CHECK_EQ(key, std::string_view("nested"));
            RUVIA_CHECK(value.is_array());
        }
        return true;
    }));
    RUVIA_CHECK_EQ(count, 3);
}

RUVIA_TEST(json_value_dynamic_traversal_handles_whitespace_and_early_stop) {
    auto array_value = ruvia::json_value::parse("[1, \n 2,\t3]");
    int seen = 0;
    RUVIA_CHECK(array_value.for_each_element([&](const ruvia::json_value& value) {
        ++seen;
        RUVIA_CHECK(value.is_number());
        return true;
    }));
    RUVIA_CHECK_EQ(seen, 3);

    auto object = ruvia::json_value::parse("{\"plain\": 1, \n \"escaped\\u03bb\": 2}");
    RUVIA_CHECK(object.for_each_field([](std::string_view key, const ruvia::json_value&) {
        return key == "plain" || key == "escapedλ";
    }));

    int stopped = 0;
    RUVIA_CHECK(!array_value.for_each_element([&](const ruvia::json_value&) {
        ++stopped;
        return false;
    }));
    RUVIA_CHECK_EQ(stopped, 1);
}

RUVIA_TEST(json_value_dynamic_traversal_handles_empty_wrong_kind_and_early_stop) {
    auto empty = ruvia::json_value::parse("[]");
    auto object = ruvia::json_value::parse("{}");
    auto scalar = ruvia::json_value::parse("1");
    RUVIA_CHECK(empty.for_each_element([](const ruvia::json_value&) { return true; }));
    RUVIA_CHECK(object.for_each_field([](std::string_view, const ruvia::json_value&) { return true; }));
    RUVIA_CHECK(!scalar.for_each_element([](const ruvia::json_value&) { return true; }));

    auto array_value = ruvia::json_value::parse("[1,2,3]");
    int seen = 0;
    RUVIA_CHECK(!array_value.for_each_element([&](const ruvia::json_value&) {
        ++seen;
        return false;
    }));
    RUVIA_CHECK_EQ(seen, 1);
}

RUVIA_TEST(json_value_object_traversal_reports_completion_and_stops_once) {
    const auto empty = ruvia::json_value::parse("{}");
    const auto object = ruvia::json_value::parse(R"({"a":1,"b":2,"c":3})");
    int calls = 0;
    const auto stop = [&](std::string_view, const ruvia::json_value&) {
        ++calls;
        return false;
    };
    RUVIA_CHECK(empty.for_each_field(stop));
    RUVIA_CHECK_EQ(calls, 0);
    RUVIA_CHECK(!object.for_each_field(stop));
    RUVIA_CHECK_EQ(calls, 1);
    calls = 0;
    RUVIA_CHECK(!object.for_each_field([&](std::string_view, const ruvia::json_value&) {
        return ++calls < 2;
    }));
    RUVIA_CHECK_EQ(calls, 2);
    calls = 0;
    RUVIA_CHECK(object.for_each_field([&](std::string_view, const ruvia::json_value&) {
        ++calls;
        return true;
    }));
    RUVIA_CHECK_EQ(calls, 3);
}

RUVIA_TEST(json_value_dynamic_traversal_propagates_callback_exceptions_and_compares_tokens) {
    auto first = ruvia::json_value::parse("[1]");
    auto second = ruvia::json_value::parse("[1]");
    auto different = ruvia::json_value::parse("[1.0]");
    RUVIA_CHECK(first == second);
    RUVIA_CHECK(!(first == different));
    bool propagated = false;
    try {
        (void)first.for_each_element([](const ruvia::json_value&) -> bool {
            throw std::runtime_error("stop");
        });
    } catch (const std::runtime_error&) {
        propagated = true;
    }
    RUVIA_CHECK(propagated);
}

RUVIA_TEST(json_value_get_reads_root_scalars_and_arrays_and_models) {
    auto string_value = ruvia::json_value::parse("\"hello\"");
    auto boolean = ruvia::json_value::parse("true");
    RUVIA_CHECK_EQ(string_value.get<ruvia::string>()->view(), std::string_view("hello"));
    RUVIA_CHECK(*boolean.get<ruvia::bool_value>());

    auto valid_model = ruvia::json_value::parse(R"({"child":{"name":"ada"},"items":[{"name":"grace"}]})");
    RUVIA_CHECK(valid_model.get<json_object_envelope>().has_value());
    auto missing_required = ruvia::json_value::parse(R"({"child":{}})");
    RUVIA_CHECK(!missing_required.get<json_object_envelope>().has_value());
    auto invalid_nested_array = ruvia::json_value::parse(R"({"child":{"name":"ada"},"items":[{}]})");
    RUVIA_CHECK(!invalid_nested_array.get<json_object_envelope>().has_value());

    auto number = ruvia::json_value::parse("9007199254740993");
    const auto value = number.get<ruvia::uint64>();
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(static_cast<std::uint64_t>(*value), 9007199254740993ULL);

    auto array_value = ruvia::json_value::parse("[1,2]");
    const auto parsed_value = array_value.get<ruvia::array<ruvia::int32>>();
    RUVIA_CHECK(parsed_value.has_value());
    RUVIA_CHECK_EQ(parsed_value->size(), 2U);
    RUVIA_CHECK(!number.get<ruvia::array<ruvia::int32>>().has_value());
}

RUVIA_TEST(json_value_dynamic_traversal_reclaims_decoded_key_on_exception) {
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
    RUVIA_CHECK(thrown);
    RUVIA_CHECK_EQ(resource.live_allocations(), 0U);
}

RUVIA_TEST(json_value_get_reads_object_fields_and_rejects_non_objects) {
    auto json =
        ruvia::json_value::parse(R"({"age":1,"age":2})", {.resource_ = std::pmr::get_default_resource()});
    const auto age = json.get<ruvia::int32>("age");
    RUVIA_CHECK(age.has_value());
    RUVIA_CHECK_EQ(static_cast<std::int32_t>(*age), 2);
    RUVIA_CHECK(!json.get<ruvia::string>("missing").has_value());

    auto array_value = ruvia::json_value::parse("[1]");
    RUVIA_CHECK(!array_value.get<ruvia::int32>("age").has_value());
}

RUVIA_TEST(json_dynamic_parse_enforces_cumulative_array_element_budget) {
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse("[1]", {.max_array_elements_ = 0}); }));
    RUVIA_CHECK(!ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse("[]", {.max_array_elements_ = 0}); }));
    RUVIA_CHECK(!ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse("[1]", {.max_array_elements_ = 1}); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse("[[1],[2]]", {.max_array_elements_ = 3}); }));
    RUVIA_CHECK(!ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse("[[1],[2]]", {.max_array_elements_ = 4}); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse(R"({"a":{"b":[1,2]}})", {.max_array_elements_ = 1}); }));

    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse(R"({"a":[1]})", {.max_array_elements_ = 0}); }));
    RUVIA_CHECK(!ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse(R"({"a":[1]})", {.max_array_elements_ = 1}); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        (void)ruvia::json_object::parse(R"({"a":{"b":[1,2]},"c":[3]})", {.max_array_elements_ = 2});
    }));
    RUVIA_CHECK(!ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse(" {} ", {.max_array_elements_ = 0}); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse("[]"); }));

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
    RUVIA_CHECK(!ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse(at_default_limit); }));
    RUVIA_CHECK(!ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse(at_default_limit); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::json_value::parse(over_default_limit); }));
    RUVIA_CHECK(ruvia::testing::throws_on([&] { (void)ruvia::json_object::parse(over_default_limit); }));
}
