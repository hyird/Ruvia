#include <cstddef>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/model.h"
#include "ruvia/web/model_json.h"

#include "test_harness.h"

namespace {

RUVIA_MODEL(remark_request, RUVIA_OPTIONAL_FIELD(remark, ruvia::string, RUVIA_NULLABLE));

RUVIA_MODEL(remark_with_default_request,
    RUVIA_OPTIONAL_FIELD(remark, ruvia::string, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback")));

RUVIA_MODEL(required_remark_request, RUVIA_REQUIRED_FIELD(remark, ruvia::string));

RUVIA_MODEL(json_bag_request, RUVIA_OPTIONAL_FIELD(payload, ruvia::json_value, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(items, ruvia::array<ruvia::json_value>));

RUVIA_MODEL(json_bag_response, RUVIA_OPTIONAL_FIELD(payload, ruvia::json_value),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object));

RUVIA_MODEL(emission_options,
    RUVIA_OPTIONAL_FIELD_NAME("p\"lain", plain, ruvia::string, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(omitted, ruvia::string, RUVIA_NULLABLE, RUVIA_OMIT_EMPTY),
    RUVIA_OPTIONAL_FIELD(emitted, ruvia::string, RUVIA_NULLABLE, RUVIA_EMIT_NULL),
    RUVIA_OPTIONAL_FIELD(both, ruvia::string, RUVIA_NULLABLE, RUVIA_OMIT_EMPTY, RUVIA_EMIT_NULL));

}  // namespace

RUVIA_TEST(nullable_optional_string_accepts_json_null) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed_value =
        ruvia::from_json<remark_request>(R"({"remark":null})", {.resource_ = &resource});
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    RUVIA_CHECK(!parsed_value->get<"remark">().has_value());
    RUVIA_CHECK(parsed_value->is_present<"remark">());
    RUVIA_CHECK(parsed_value->is_null<"remark">());
}

RUVIA_TEST(nullable_optional_string_still_accepts_text) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed_value =
        ruvia::from_json<remark_request>(R"({"remark":"note"})", {.resource_ = &resource});
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value || !parsed_value->get<"remark">()) {
        return;
    }
    RUVIA_CHECK_EQ(parsed_value->get<"remark">()->view(), std::string_view("note"));
}

RUVIA_TEST(nullable_null_does_not_apply_default) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed_value = ruvia::from_json<remark_with_default_request>(
        R"({"remark":null})", {.resource_ = &resource});
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    RUVIA_CHECK(!parsed_value->get<"remark">().has_value());
    RUVIA_CHECK(parsed_value->is_present<"remark">());
    RUVIA_CHECK(parsed_value->is_null<"remark">());

    const auto missing =
        ruvia::from_json<remark_with_default_request>("{}", {.resource_ = &resource});
    RUVIA_CHECK(missing.has_value());
    if (!missing || !missing->get<"remark">()) {
        return;
    }
    RUVIA_CHECK_EQ(missing->get<"remark">()->view(), std::string_view("fallback"));
}

RUVIA_TEST(required_string_rejects_json_null) {
    std::pmr::monotonic_buffer_resource resource;
    RUVIA_CHECK(!ruvia::from_json<required_remark_request>(R"({"remark":null})", {.resource_ = &resource})
            .has_value());
}

RUVIA_TEST(json_value_model_fields_accept_any_json_token) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed_value = ruvia::from_json<json_bag_request>(
        R"({"payload":[1,{"k":null}],"object":{"a":true},"items":[null,"x",{"z":2}]})",
        {.resource_ = &resource});
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value || !parsed_value->get<"payload">() || !parsed_value->get<"object">() ||
        !parsed_value->get<"items">()) {
        return;
    }
    RUVIA_CHECK(parsed_value->get<"payload">()->is_array());
    RUVIA_CHECK_EQ(parsed_value->get<"payload">()->view(), std::string_view(R"([1,{"k":null}])"));
    RUVIA_CHECK_EQ(parsed_value->get<"object">()->view(), std::string_view(R"({"a":true})"));
    const auto& items = *parsed_value->get<"items">();
    RUVIA_CHECK_EQ(items.size(), std::size_t{3});
    RUVIA_CHECK(items[0].is_null());
    RUVIA_CHECK(items[1].is_string());
}

RUVIA_TEST(json_object_model_field_rejects_non_objects) {
    std::pmr::monotonic_buffer_resource resource;
    RUVIA_CHECK(!ruvia::from_json<json_bag_request>(R"({"object":[1]})", {.resource_ = &resource})
            .has_value());

    const auto null_object =
        ruvia::from_json<json_bag_request>(R"({"object":null})", {.resource_ = &resource});
    RUVIA_CHECK(null_object.has_value());
    if (null_object) {
        RUVIA_CHECK(!null_object->get<"object">().has_value());
        RUVIA_CHECK(null_object->is_present<"object">());
        RUVIA_CHECK(null_object->is_null<"object">());
    }
}

RUVIA_TEST(json_value_model_fields_own_tokens_for_from_json) {
    std::pmr::monotonic_buffer_resource resource;
    auto parsed_value = ruvia::from_json<json_bag_request>(
        std::string(R"({"payload":{"keep":true}})"), {.resource_ = &resource});
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value || !parsed_value->get<"payload">()) {
        return;
    }
    RUVIA_CHECK(parsed_value->get<"payload">()->is_object());
    RUVIA_CHECK_EQ(parsed_value->get<"payload">()->view(), std::string_view(R"({"keep":true})"));
}

RUVIA_TEST(json_value_response_fields_write_raw_tokens) {
    std::pmr::monotonic_buffer_resource resource;
    auto parsed_value = ruvia::from_json<json_bag_request>(
        R"({"payload":[1,2],"object":{"a":1}})", {.resource_ = &resource});
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value || !parsed_value->get<"payload">() || !parsed_value->get<"object">()) {
        return;
    }

    json_bag_response response({.resource_ = &resource});
    response.set<"payload">(std::move(parsed_value->ensure<"payload">()));
    response.set<"object">(std::move(parsed_value->ensure<"object">()));
    RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(response, {.resource_ = &resource})),
        std::string_view(R"({"payload":[1,2],"object":{"a":1}})"));
}

RUVIA_TEST(model_json_emission_options_write_expected_output_for_all_states) {
    // Each pair of bits selects missing, explicit null, empty, or text. The
    // Cartesian product also covers every comma position after omitted fields.
    constexpr std::string_view names[] = {R"("p\"lain")", R"("omitted")", R"("emitted")", R"("both")"};
    for (unsigned combination = 0; combination != 256; ++combination) {
        emission_options value;
        const auto assign = [&]<ruvia::fixed_string field>(unsigned state_value) {
            if (state_value == 1) {
                value.set<field>(nullptr);
            } else if (state_value == 2) {
                value.set<field>("");
            } else if (state_value == 3) {
                value.set<field>("\"\n");
            }
        };
        assign.template operator()<"plain">(combination & 3);
        assign.template operator()<"omitted">((combination >> 2) & 3);
        assign.template operator()<"emitted">((combination >> 4) & 3);
        assign.template operator()<"both">((combination >> 6) & 3);

        std::string expected("{");
        for (unsigned index = 0; index != 4; ++index) {
            const auto state_value = (combination >> (index * 2)) & 3;
            if ((state_value == 0 && index < 2) || (state_value == 2 && (index == 1 || index == 3))) {
                continue;
            }
            if (expected.size() != 1) {
                expected.push_back(',');
            }
            expected.append(names[index]);
            expected.push_back(':');
            expected.append(state_value < 2 ? "null" : state_value == 2 ? R"("")"
                                                                        : R"("\"\n")");
        }
        expected.push_back('}');
        const auto output = ruvia::to_json(value);
        RUVIA_CHECK_EQ(std::string_view(output), std::string_view(expected));
    }
}
