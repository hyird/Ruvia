#include <memory_resource>
#include <string_view>

#include "ruvia/web/ModelObject.h"

#include "test_harness.h"

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
