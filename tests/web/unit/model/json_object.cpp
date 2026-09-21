#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/web/Model.h"
#include "ruvia/web/ModelObject.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

RUVIA_MODEL(JsonObjectChild, RUVIA_REQUIRED_FIELD(name, ruvia::String));
RUVIA_MODEL(JsonObjectEnvelope,
    RUVIA_REQUIRED_FIELD(child, JsonObjectChild),
    RUVIA_REQUIRED_FIELD(items, ruvia::Array<JsonObjectChild>));

RUVIA_TEST(json_value_parse_classifies_kinds_and_rejects_trailing_input) {
    const auto object = ruvia::JsonValue::parse(R"({"name":"ada"})");
    RUVIA_CHECK(object.has_value());
    RUVIA_CHECK(object->isObject());
    RUVIA_CHECK(!object->isArray());
    RUVIA_CHECK(!object->isNull());
    RUVIA_CHECK_EQ(object->view(), std::string_view(R"({"name":"ada"})"));

    const auto array = ruvia::JsonValue::parse("[1,2]");
    RUVIA_CHECK(array.has_value());
    RUVIA_CHECK(array->isArray());

    const auto nullValue = ruvia::JsonValue::parse("null");
    RUVIA_CHECK(nullValue.has_value());
    RUVIA_CHECK(nullValue->isNull());

    RUVIA_CHECK(!ruvia::JsonValue::parse(R"({"name":"ada"} trailing)").has_value());
    RUVIA_CHECK(!ruvia::JsonValue::parse("{").has_value());
}

RUVIA_TEST(json_object_parse_requires_a_complete_object) {
    const auto json = ruvia::JsonObject::parse(R"({"name":"ada"})");
    RUVIA_CHECK(json.has_value());
    RUVIA_CHECK_EQ(json->view(), std::string_view(R"({"name":"ada"})"));

    RUVIA_CHECK(!ruvia::JsonObject::parse("[1,2]").has_value());
    RUVIA_CHECK(!ruvia::JsonObject::parse("null").has_value());
    RUVIA_CHECK(!ruvia::JsonObject::parse(R"({"name":"ada"} 1)").has_value());
}

RUVIA_TEST(json_object_get_uses_last_match) {
    auto json = ruvia::JsonObject::parse(R"({"name":"first","other":"x","name":"second"})",
        {.resource = std::pmr::get_default_resource()});
    RUVIA_CHECK(json.has_value());

    const auto value = json->get<ruvia::String>("name");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(value->view(), std::string_view("second"));
}

RUVIA_TEST(json_value_dynamic_traversal_decodes_keys_and_preserves_order) {
    auto json = ruvia::JsonValue::parse(R"({"a\u0062":1,"a\u0062":2,"nested":[3,4]})");
    RUVIA_CHECK(json.has_value());
    int count = 0;
    RUVIA_CHECK(json->forEachField([&](std::string_view key, const ruvia::JsonValue& value) {
        ++count;
        if (count < 3) {
            RUVIA_CHECK_EQ(key, std::string_view("ab"));
            RUVIA_CHECK(value.isNumber());
        } else {
            RUVIA_CHECK_EQ(key, std::string_view("nested"));
            RUVIA_CHECK(value.isArray());
        }
        return true;
    }));
    RUVIA_CHECK_EQ(count, 3);
}

RUVIA_TEST(json_value_dynamic_traversal_handles_whitespace_and_early_stop) {
    auto array = ruvia::JsonValue::parse("[1, \n 2,\t3]");
    RUVIA_CHECK(array.has_value());
    int seen = 0;
    RUVIA_CHECK(array->forEachElement([&](const ruvia::JsonValue& value) {
        ++seen;
        RUVIA_CHECK(value.isNumber());
        return true;
    }));
    RUVIA_CHECK_EQ(seen, 3);

    auto object = ruvia::JsonValue::parse("{\"plain\": 1, \n \"escaped\\u03bb\": 2}");
    RUVIA_CHECK(object.has_value());
    RUVIA_CHECK(object->forEachField([](std::string_view key, const ruvia::JsonValue&) {
        return key == "plain" || key == "escapedλ";
    }));

    int stopped = 0;
    RUVIA_CHECK(!array->forEachElement([&](const ruvia::JsonValue&) {
        ++stopped;
        return false;
    }));
    RUVIA_CHECK_EQ(stopped, 1);
}

RUVIA_TEST(json_value_dynamic_traversal_handles_empty_wrong_kind_and_early_stop) {
    auto empty = ruvia::JsonValue::parse("[]");
    auto object = ruvia::JsonValue::parse("{}");
    auto scalar = ruvia::JsonValue::parse("1");
    RUVIA_CHECK(empty->forEachElement([](const ruvia::JsonValue&) { return true; }));
    RUVIA_CHECK(object->forEachField([](std::string_view, const ruvia::JsonValue&) { return true; }));
    RUVIA_CHECK(!scalar->forEachElement([](const ruvia::JsonValue&) { return true; }));

    auto array = ruvia::JsonValue::parse("[1,2,3]");
    int seen = 0;
    RUVIA_CHECK(!array->forEachElement([&](const ruvia::JsonValue&) {
        ++seen;
        return false;
    }));
    RUVIA_CHECK_EQ(seen, 1);
}

RUVIA_TEST(json_value_dynamic_traversal_propagates_callback_exceptions_and_compares_tokens) {
    auto first = ruvia::JsonValue::parse("[1]");
    auto second = ruvia::JsonValue::parse("[1]");
    auto different = ruvia::JsonValue::parse("[1.0]");
    RUVIA_CHECK(*first == *second);
    RUVIA_CHECK(!(*first == *different));
    bool propagated = false;
    try {
        (void)first->forEachElement([](const ruvia::JsonValue&) -> bool {
            throw std::runtime_error("stop");
        });
    } catch (const std::runtime_error&) {
        propagated = true;
    }
    RUVIA_CHECK(propagated);
}

RUVIA_TEST(json_value_get_reads_root_scalars_and_arrays_and_models) {
    auto string = ruvia::JsonValue::parse("\"hello\"");
    auto boolean = ruvia::JsonValue::parse("true");
    RUVIA_CHECK_EQ(string->get<ruvia::String>()->view(), std::string_view("hello"));
    RUVIA_CHECK(*boolean->get<ruvia::Bool>());

    auto validModel = ruvia::JsonValue::parse(R"({"child":{"name":"ada"},"items":[{"name":"grace"}]})");
    RUVIA_CHECK(validModel->get<JsonObjectEnvelope>().has_value());
    auto missingRequired = ruvia::JsonValue::parse(R"({"child":{}})");
    RUVIA_CHECK(!missingRequired->get<JsonObjectEnvelope>().has_value());
    auto invalidNestedArray = ruvia::JsonValue::parse(R"({"child":{"name":"ada"},"items":[{}]})");
    RUVIA_CHECK(!invalidNestedArray->get<JsonObjectEnvelope>().has_value());

    auto number = ruvia::JsonValue::parse("9007199254740993");
    RUVIA_CHECK(number.has_value());
    const auto value = number->get<ruvia::UInt64>();
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(static_cast<std::uint64_t>(*value), 9007199254740993ULL);

    auto array = ruvia::JsonValue::parse("[1,2]");
    RUVIA_CHECK(array.has_value());
    const auto parsed = array->get<ruvia::Array<ruvia::Int32>>();
    RUVIA_CHECK(parsed.has_value());
    RUVIA_CHECK_EQ(parsed->size(), 2U);
    RUVIA_CHECK(!number->get<ruvia::Array<ruvia::Int32>>().has_value());
}

RUVIA_TEST(json_value_dynamic_traversal_reclaims_decoded_key_on_exception) {
    ruvia::test::CountingMemoryResource resource;
    std::string body = "{\"" + std::string(256, 'k') + "\\u0061\":1}";
    const auto json = ruvia::JsonValue::parse(body, {.resource = &resource});
    RUVIA_CHECK(json.has_value());
    bool thrown = false;
    try {
        (void)json->forEachField([](std::string_view, const ruvia::JsonValue&) -> bool {
            throw std::runtime_error("visitor");
        });
    } catch (const std::runtime_error&) {
        thrown = true;
    }
    RUVIA_CHECK(thrown);
    RUVIA_CHECK_EQ(resource.liveAllocations(), 0U);
}

RUVIA_TEST(json_value_get_reads_object_fields_and_rejects_non_objects) {
    auto json =
        ruvia::JsonValue::parse(R"({"age":1,"age":2})", {.resource = std::pmr::get_default_resource()});
    RUVIA_CHECK(json.has_value());
    const auto age = json->get<ruvia::Int32>("age");
    RUVIA_CHECK(age.has_value());
    RUVIA_CHECK_EQ(static_cast<std::int32_t>(*age), 2);
    RUVIA_CHECK(!json->get<ruvia::String>("missing").has_value());

    auto array = ruvia::JsonValue::parse("[1]");
    RUVIA_CHECK(array.has_value());
    RUVIA_CHECK(!array->get<ruvia::Int32>("age").has_value());
}
