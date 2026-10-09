#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/model.h"
#include "ruvia/web/model_form.h"
#include "ruvia/web/model_json.h"
#include "ruvia/web/validation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

RUVIA_MODEL(required_value, RUVIA_REQUIRED_FIELD(value, ruvia::string));
RUVIA_MODEL(optional_value, RUVIA_OPTIONAL_FIELD(value, ruvia::string));
RUVIA_MODEL(required_nullable_value,
    RUVIA_REQUIRED_FIELD(value, ruvia::string, RUVIA_NULLABLE));
RUVIA_MODEL(optional_nullable_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_NULLABLE));
RUVIA_MODEL(default_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_DEFAULT("fallback"), RUVIA_MIN(2, "too short")));
RUVIA_MODEL(nullable_default_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::string, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback"), RUVIA_MIN(2, "too short")));
RUVIA_MODEL(invalid_default_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::uint32, RUVIA_DEFAULT(3), RUVIA_MIN(5, "too small")));
RUVIA_MODEL(required_default_value,
    RUVIA_REQUIRED_FIELD(value, ruvia::string, RUVIA_DEFAULT("fallback")));
RUVIA_MODEL(named_value,
    RUVIA_OPTIONAL_FIELD_NAME("wire_value", value, ruvia::string, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback")));
RUVIA_MODEL(patch_value,
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::bool_value),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::string, RUVIA_NULLABLE));
RUVIA_MODEL(nested_values,
    RUVIA_REQUIRED_FIELD(children, ruvia::array<nullable_default_value>));
RUVIA_MODEL(nullable_kinds,
    RUVIA_REQUIRED_FIELD(flag, ruvia::bool_value, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(number, ruvia::uint32, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(child, required_value, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(items, ruvia::array<ruvia::string>, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(boxes, ruvia::boxed_array<required_value>, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(payload, ruvia::json_value, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(object, ruvia::json_object, RUVIA_NULLABLE));
RUVIA_MODEL(dynamic_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_value));
RUVIA_MODEL(dynamic_object,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_object));
RUVIA_MODEL(owned_values,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_value, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(note, ruvia::string, RUVIA_DEFAULT("a default string long enough to allocate storage")));
RUVIA_MODEL(nullable_response,
    RUVIA_REQUIRED_FIELD(required, ruvia::string, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(optional, ruvia::string, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(dynamic, ruvia::json_value, RUVIA_NULLABLE));

template <typename model_type>
void check_presence_matrix(ruvia::testing::test_context& ruvia_ctx, bool required, bool nullable_value) {
    for (auto input : {R"({"value":42})", R"({"value":false})", R"({"value":[]})", R"({"value":{}})"}) {
        RUVIA_CHECK(!ruvia::from_json<model_type>(input));
    }
    for (auto input : {R"({"value":null,"value":"note"})",
             R"({"value":"note","value":null})", R"({"value":null,"value":null})",
             R"({"value":42,"value":"note"})", R"({"value":"note","va\u006cue":"again"})"}) {
        RUVIA_CHECK(!ruvia::from_json<model_type>(input));
    }
    RUVIA_CHECK_EQ(ruvia::from_json<model_type>("{}").has_value(), !required);
    RUVIA_CHECK_EQ(ruvia::from_json<model_type>(R"({"value":null})").has_value(), nullable_value);
    RUVIA_CHECK(ruvia::from_json<model_type>(R"({"value":"note"})").has_value());
}

}  // namespace

RUVIA_TEST(model_field_presence_and_nullability_are_independent) {
    check_presence_matrix<required_value>(ruvia_ctx, true, false);
    check_presence_matrix<optional_value>(ruvia_ctx, false, false);
    check_presence_matrix<required_nullable_value>(ruvia_ctx, true, true);
    check_presence_matrix<optional_nullable_value>(ruvia_ctx, false, true);
    auto null = ruvia::from_json<required_nullable_value>(R"({"value":null})");
    auto text = ruvia::from_json<required_nullable_value>(R"({"value":"note"})");
    RUVIA_CHECK(null && !null->get<"value">());
    RUVIA_CHECK(text && text->get<"value">()->view() == "note");
}

RUVIA_TEST(model_patch_public_states_distinguish_omission_clear_and_assignment) {
    std::optional<std::string> remark{"original"};
    auto apply = [&](std::string_view json) {
        const auto patch = ruvia::from_json<patch_value>(json);
        RUVIA_CHECK(patch.has_value());
        if (!patch || !patch->is_present<"remark">()) {
            return;
        }
        if (patch->is_null<"remark">()) {
            remark.reset();
        } else {
            remark = patch->get<"remark">()->view();
        }
    };
    apply("{}");
    RUVIA_CHECK(remark == "original");
    apply(R"({"remark":null})");
    RUVIA_CHECK(!remark);
    apply(R"({"remark":"replacement"})");
    RUVIA_CHECK(remark == "replacement");
    apply(R"({"remark":""})");
    RUVIA_CHECK(remark && remark->empty());
    RUVIA_CHECK(!ruvia::from_json<patch_value>(R"({"enabled":null})"));
    const auto disabled = ruvia::from_json<patch_value>(R"({"enabled":false})");
    RUVIA_CHECK(disabled && disabled->is_present<"enabled">() && !disabled->is_null<"enabled">());
    RUVIA_CHECK(disabled && !bool(*disabled->get<"enabled">()));
}

RUVIA_TEST(model_defaults_only_fill_optional_missing_values) {
    RUVIA_CHECK(!ruvia::from_json<required_default_value>("{}"));
    RUVIA_CHECK(!ruvia::from_form<required_default_value>(""));
    auto missing = ruvia::from_json<nullable_default_value>("{}");
    RUVIA_CHECK(missing && missing->get<"value">()->view() == "fallback");
    RUVIA_CHECK(missing && !missing->is_present<"value">());
    auto form = ruvia::from_form<default_value>("");
    RUVIA_CHECK(form && !form->is_present<"value">() && form->get<"value">()->view() == "fallback");
    auto invalid = ruvia::from_form<invalid_default_value>("");
    RUVIA_CHECK(invalid.has_value());
}

RUVIA_TEST(model_input_presence_survives_application_mutation_and_moves) {
    auto absent = ruvia::from_json<nullable_default_value>("{}");
    auto present = ruvia::from_json<nullable_default_value>(R"({"value":null})");
    RUVIA_CHECK(absent && present);
    if (!absent || !present) {
        return;
    }
    absent->set<"value">("application value");
    RUVIA_CHECK(!absent->is_present<"value">());
    present->ensure<"value">().assign_owned(std::string_view("replacement"));
    RUVIA_CHECK(present->is_present<"value">() && !present->is_null<"value">());
    present->reset<"value">();
    RUVIA_CHECK(present->is_present<"value">() && !present->is_null<"value">());
    present->set<"value">(nullptr);
    nullable_default_value moved(std::move(*present));
    RUVIA_CHECK(moved.is_present<"value">() && moved.is_null<"value">());
    RUVIA_CHECK(!present->is_present<"value">() && !present->is_null<"value">());
    *absent = std::move(moved);
    RUVIA_CHECK(absent->is_present<"value">() && absent->is_null<"value">());
    RUVIA_CHECK(!moved.is_present<"value">());
}

RUVIA_TEST(model_named_form_and_nested_fields_keep_input_presence) {
    auto named = ruvia::from_json<named_value>(R"({"wire_value":null})");
    RUVIA_CHECK(named && named->is_present<"value">() && named->is_null<"value">());
    auto text = ruvia::from_form<named_value>("wire_value=null");
    RUVIA_CHECK(text && text->is_present<"value">() && !text->is_null<"value">());
    RUVIA_CHECK(text && text->get<"value">()->view() == "null");
    RUVIA_CHECK(!ruvia::from_form<named_value>("wire_value=a&wire_value=b"));
    auto nested = ruvia::from_json<nested_values>(R"({"children":[{},{"value":null},{"value":"note"}]})");
    RUVIA_CHECK(nested.has_value());
    if (!nested) {
        return;
    }
    const auto& children = nested->get<"children">();
    RUVIA_CHECK(!children[0].is_present<"value">() && children[0].get<"value">()->view() == "fallback");
    RUVIA_CHECK(children[1].is_present<"value">() && children[1].is_null<"value">());
    RUVIA_CHECK(children[2].is_present<"value">() && !children[2].is_null<"value">());
}

RUVIA_TEST(model_nullable_applies_to_scalar_container_nested_and_dynamic_types) {
    auto parsed_value = ruvia::from_json<nullable_kinds>(
        R"({"flag":null,"number":null,"child":null,"items":null,"boxes":null,"payload":null,"object":null})");
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    RUVIA_CHECK(parsed_value->is_present<"flag">() && parsed_value->is_null<"flag">() && !parsed_value->get<"flag">());
    RUVIA_CHECK(parsed_value->is_null<"number">() && !parsed_value->get<"number">());
    RUVIA_CHECK(parsed_value->is_null<"child">() && !parsed_value->get<"child">());
    RUVIA_CHECK(parsed_value->is_null<"items">() && !parsed_value->get<"items">());
    RUVIA_CHECK(parsed_value->is_null<"boxes">() && !parsed_value->get<"boxes">());
    RUVIA_CHECK(parsed_value->is_null<"payload">() && !parsed_value->get<"payload">());
    RUVIA_CHECK(parsed_value->is_null<"object">() && !parsed_value->get<"object">());
    RUVIA_CHECK(!ruvia::from_json<dynamic_value>(R"({"value":null})"));
    RUVIA_CHECK(!ruvia::from_json<dynamic_object>(R"({"value":null})"));
    RUVIA_CHECK(!ruvia::from_json<dynamic_object>(R"({"value":[]})"));
    RUVIA_CHECK(ruvia::from_json<dynamic_value>(R"({"value":{"nested":null}})").has_value());
    RUVIA_CHECK(ruvia::from_json<dynamic_value>(R"({"value":[null,false,1]})").has_value());
    RUVIA_CHECK(ruvia::from_json<dynamic_object>(R"({"value":{"nested":null}})").has_value());
}

RUVIA_TEST(model_nonnullable_dynamic_assignment_rejects_null_without_changing_value) {
    auto model = ruvia::from_json<dynamic_value>(R"({"value":1})");
    RUVIA_CHECK(model.has_value());
    if (!model) {
        return;
    }
    auto null = ruvia::json_value::parse("null");
    bool rejected = false;
    try {
        model->set<"value">(std::move(*null));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    RUVIA_CHECK(rejected);
    RUVIA_CHECK(model->is_present<"value">());
    RUVIA_CHECK(!model->is_null<"value">());
    RUVIA_CHECK_EQ(model->get<"value">()->view(), std::string_view("1"));
    RUVIA_CHECK(null->is_null());
}

RUVIA_TEST(model_nullable_response_emits_explicit_null_without_emitting_absence) {
    nullable_response response;
    response.set<"required">(nullptr);
    RUVIA_CHECK(response.is_null<"required">());
    RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(response)), std::string_view(R"({"required":null})"));
    response.set<"optional">(nullptr);
    auto token = ruvia::json_value::parse("null");
    response.set<"dynamic">(std::move(*token));
    RUVIA_CHECK(response.is_null<"dynamic">());
    RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(response)),
        std::string_view(R"({"required":null,"optional":null,"dynamic":null})"));
    response.reset<"optional">();
    response.set<"required">("note");
    RUVIA_CHECK(!response.is_null<"required">());
    RUVIA_CHECK_EQ(std::string_view(ruvia::to_json(response)),
        std::string_view(R"({"required":"note","dynamic":null})"));
}

RUVIA_TEST(model_dynamic_owned_tokens_and_presence_survive_rebinding_and_reclaim_memory) {
    ruvia::test::counting_memory_resource source;
    ruvia::test::counting_memory_resource destination;
    const std::string text(256, 'x');
    const std::string body = "{\"value\":{\"text\":\"" + text + "\"},\"object\":{\"text\":\"" + text + "\"}}";
    {
        owned_values retained({.resource_ = &destination});
        {
            auto input = body;
            auto parsed_value = ruvia::from_json<owned_values>(input, {.resource_ = &source});
            RUVIA_CHECK(parsed_value.has_value());
            if (!parsed_value) {
                return;
            }
            input.assign(input.size(), '?');
            owned_values moved(std::move(*parsed_value));
            RUVIA_CHECK(moved.is_present<"value">() && !moved.is_present<"note">());
            retained = std::move(moved);
        }
        RUVIA_CHECK_EQ(source.live_allocations(), std::size_t{0});
        RUVIA_CHECK(retained.is_present<"value">() && retained.is_present<"object">());
        RUVIA_CHECK(!retained.is_present<"note">() && retained.get<"note">().has_value());
        RUVIA_CHECK(retained.get<"value">()->get<ruvia::string>("text")->view() == text);
        const auto baseline = destination.live_allocations();
        for (int index = 0; index < 32; ++index) {
            {
                auto parsed_value = ruvia::from_json<owned_values>(body, {.resource_ = &destination});
                RUVIA_CHECK(parsed_value.has_value());
            }
            RUVIA_CHECK_EQ(destination.live_allocations(), baseline);
            for (const auto& invalid : {body.substr(0, body.size() - 1) + ",\"value\":null}",
                     body.substr(0, body.size() - 1) + ",\"note\":42}", body.substr(0, body.size() - 1)}) {
                RUVIA_CHECK(!ruvia::from_json<owned_values>(invalid, {.resource_ = &destination}));
                RUVIA_CHECK_EQ(destination.live_allocations(), baseline);
            }
            RUVIA_CHECK(retained.get<"object">()->get<ruvia::string>("text")->view() == text);
            RUVIA_CHECK_EQ(destination.live_allocations(), baseline);
        }
        RUVIA_CHECK(destination.deallocation_count() > 0);
    }
    RUVIA_CHECK_EQ(destination.live_allocations(), std::size_t{0});
}
