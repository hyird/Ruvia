#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/model.h"
#include "ruvia/web/validation.h"

#include "memory_resource_fixture.h"
#include "test_exceptions.h"

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
RUVIA_MODEL(nullable_dynamic_value,
    RUVIA_OPTIONAL_FIELD(value, ruvia::json_value, RUVIA_NULLABLE));
RUVIA_MODEL(required_dynamic_value,
    RUVIA_REQUIRED_FIELD(value, ruvia::json_value));
RUVIA_MODEL(required_nullable_dynamic_value,
    RUVIA_REQUIRED_FIELD(value, ruvia::json_value, RUVIA_NULLABLE));
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
RUVIA_MODEL(form_unrepresentable_values,
    RUVIA_OPTIONAL_FIELD(tags, ruvia::array<ruvia::string>),
    RUVIA_OPTIONAL_FIELD(data, ruvia::bytes),
    RUVIA_OPTIONAL_FIELD(child, required_value),
    RUVIA_OPTIONAL_FIELD(payload, ruvia::json_value),
    RUVIA_OPTIONAL_FIELD(object, ruvia::json_object),
    RUVIA_OPTIONAL_FIELD(note, ruvia::string));

// Models spell value presence only as has_value; entity is_set means "assigned,
// null included" and must not appear on models with a different meaning.
template <typename model_type>
concept model_offers_has_value = requires(const model_type& model) { model.template has_value<"optional">(); };
template <typename model_type>
concept model_offers_is_set = requires(const model_type& model) { model.template is_set<"optional">(); };
static_assert(model_offers_has_value<nullable_response>);
static_assert(!model_offers_is_set<nullable_response>);

template <typename model_type>
void check_presence_matrix(bool required, bool nullable_value) {
    for (auto input : {R"({"value":42})", R"({"value":false})", R"({"value":[]})", R"({"value":{}})"}) {
        EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_json<model_type>(input); }));
    }
    for (auto input : {R"({"value":null,"value":"note"})",
             R"({"value":"note","value":null})", R"({"value":null,"value":null})",
             R"({"value":42,"value":"note"})", R"({"value":"note","va\u006cue":"again"})"}) {
        EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_json<model_type>(input); }));
    }
    EXPECT_EQ(ruvia::testing::throws_on([&] { (void)ruvia::from_json<model_type>("{}"); }), required);
    EXPECT_EQ(
        ruvia::testing::throws_on([&] { (void)ruvia::from_json<model_type>(R"({"value":null})"); }),
        !nullable_value);
    EXPECT_TRUE(!ruvia::testing::throws_on([&] { (void)ruvia::from_json<model_type>(R"({"value":"note"})"); }));
}

}  // namespace

TEST(Ruvia, model_field_presence_and_nullability_are_independent) {
    check_presence_matrix<required_value>(true, false);
    check_presence_matrix<optional_value>(false, false);
    check_presence_matrix<required_nullable_value>(true, true);
    check_presence_matrix<optional_nullable_value>(false, true);
    auto null = ruvia::from_json<required_nullable_value>(R"({"value":null})");
    auto text = ruvia::from_json<required_nullable_value>(R"({"value":"note"})");
    EXPECT_TRUE(!null.get<"value">());
    EXPECT_TRUE(text.get<"value">()->view() == "note");
}

TEST(Ruvia, model_patch_public_states_distinguish_omission_clear_and_assignment) {
    std::optional<std::string> remark{"original"};
    auto apply = [&](std::string_view json) {
        const auto patch = ruvia::from_json<patch_value>(json);
        if (!patch.is_present<"remark">()) {
            return;
        }
        if (patch.is_null<"remark">()) {
            remark.reset();
        } else {
            remark = patch.get<"remark">()->view();
        }
    };
    apply("{}");
    EXPECT_TRUE(remark == "original");
    apply(R"({"remark":null})");
    EXPECT_TRUE(!remark);
    apply(R"({"remark":"replacement"})");
    EXPECT_TRUE(remark == "replacement");
    apply(R"({"remark":""})");
    EXPECT_TRUE(remark && remark->empty());
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_json<patch_value>(R"({"enabled":null})"); }));
    const auto disabled = ruvia::from_json<patch_value>(R"({"enabled":false})");
    EXPECT_TRUE(disabled.is_present<"enabled">() && !disabled.is_null<"enabled">());
    EXPECT_TRUE(!bool(*disabled.get<"enabled">()));
}

TEST(Ruvia, model_defaults_only_fill_optional_missing_values) {
    auto missing = ruvia::from_json<nullable_default_value>("{}");
    EXPECT_TRUE(missing.get<"value">()->view() == "fallback");
    EXPECT_TRUE(!missing.is_present<"value">());
    auto form = ruvia::from_form<default_value>("");
    EXPECT_TRUE(!form.is_present<"value">() && form.get<"value">()->view() == "fallback");
    bool default_rule_rejected = false;
    try {
        (void)ruvia::from_form<invalid_default_value>("");
    } catch (const ruvia::http_error&) {
        default_rule_rejected = true;
    }
    EXPECT_TRUE(default_rule_rejected);
}

TEST(Ruvia, model_input_presence_survives_application_mutation_and_moves) {
    auto absent = ruvia::from_json<nullable_default_value>("{}");
    auto present = ruvia::from_json<nullable_default_value>(R"({"value":null})");
    absent.set<"value">("application value");
    EXPECT_TRUE(!absent.is_present<"value">());
    present.ensure<"value">().assign_owned(std::string_view("replacement"));
    EXPECT_TRUE(present.is_present<"value">() && !present.is_null<"value">());
    present.reset<"value">();
    EXPECT_TRUE(present.is_present<"value">() && !present.is_null<"value">());
    present.set<"value">(nullptr);
    nullable_default_value moved(std::move(present));
    EXPECT_TRUE(moved.is_present<"value">() && moved.is_null<"value">());
    EXPECT_TRUE(!present.is_present<"value">() && !present.is_null<"value">());
    absent = std::move(moved);
    EXPECT_TRUE(absent.is_present<"value">() && absent.is_null<"value">());
    EXPECT_TRUE(!moved.is_present<"value">());
}

TEST(Ruvia, model_named_form_and_nested_fields_keep_input_presence) {
    auto named = ruvia::from_json<named_value>(R"({"wire_value":null})");
    EXPECT_TRUE(named.is_present<"value">() && named.is_null<"value">());
    auto text = ruvia::from_form<named_value>("wire_value=null");
    EXPECT_TRUE(text.is_present<"value">() && !text.is_null<"value">());
    EXPECT_TRUE(text.get<"value">()->view() == "null");
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_form<named_value>("wire_value=a&wire_value=b"); }));
    auto nested = ruvia::from_json<nested_values>(R"({"children":[{},{"value":null},{"value":"note"}]})");
    const auto& children = nested.get<"children">();
    EXPECT_TRUE(!children[0].is_present<"value">() && children[0].get<"value">()->view() == "fallback");
    EXPECT_TRUE(children[1].is_present<"value">() && children[1].is_null<"value">());
    EXPECT_TRUE(children[2].is_present<"value">() && !children[2].is_null<"value">());
}

TEST(Ruvia, model_form_rejects_values_for_fields_without_form_representation) {
    auto absent = ruvia::from_form<form_unrepresentable_values>("note=a&unknown=b");
    EXPECT_TRUE(absent.is_present<"note">() && !absent.is_present<"tags">());
    for (const auto body : {"tags=a", "tags=a&tags=b", "data=a", "child=a", "payload=1", "object=%7B%7D"}) {
        EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_form<form_unrepresentable_values>(body); }));
    }
}

TEST(Ruvia, model_nullable_applies_to_scalar_container_nested_and_dynamic_types) {
    auto parsed_value = ruvia::from_json<nullable_kinds>(
        R"({"flag":null,"number":null,"child":null,"items":null,"boxes":null,"payload":null,"object":null})");
    EXPECT_TRUE(parsed_value.is_present<"flag">() && parsed_value.is_null<"flag">() && !parsed_value.get<"flag">());
    EXPECT_TRUE(parsed_value.is_null<"number">() && !parsed_value.get<"number">());
    EXPECT_TRUE(parsed_value.is_null<"child">() && !parsed_value.get<"child">());
    EXPECT_TRUE(parsed_value.is_null<"items">() && !parsed_value.get<"items">());
    EXPECT_TRUE(parsed_value.is_null<"boxes">() && !parsed_value.get<"boxes">());
    EXPECT_TRUE(parsed_value.is_null<"payload">() && !parsed_value.get<"payload">());
    EXPECT_TRUE(parsed_value.is_null<"object">() && !parsed_value.get<"object">());
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_json<dynamic_value>(R"({"value":null})"); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_json<dynamic_object>(R"({"value":null})"); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_json<dynamic_object>(R"({"value":[]})"); }));
    EXPECT_TRUE(!ruvia::testing::throws_on([&] { (void)ruvia::from_json<dynamic_value>(R"({"value":{"nested":null}})"); }));
    EXPECT_TRUE(!ruvia::testing::throws_on([&] { (void)ruvia::from_json<dynamic_value>(R"({"value":[null,false,1]})"); }));
    EXPECT_TRUE(!ruvia::testing::throws_on([&] { (void)ruvia::from_json<dynamic_object>(R"({"value":{"nested":null}})"); }));
}

TEST(Ruvia, model_nonnullable_dynamic_assignment_rejects_null_without_changing_value) {
    auto model = ruvia::from_json<dynamic_value>(R"({"value":1})");
    auto null = ruvia::json_value::parse("null");
    bool rejected = false;
    try {
        model.set<"value">(std::move(null));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_TRUE(model.is_present<"value">());
    EXPECT_TRUE(!model.is_null<"value">());
    EXPECT_EQ(model.get<"value">()->view(), std::string_view("1"));
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":1})"));
    EXPECT_TRUE(null.is_null());
}

TEST(Ruvia, model_nonnullable_dynamic_setter_preserves_absence_on_null_rejection) {
    dynamic_value model;
    bool rejected = false;
    try {
        model.set<"value">(ruvia::json_value::parse("null"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_TRUE(!model.get<"value">());
    EXPECT_TRUE(!model.is_null<"value">() && !model.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view("{}"));

    model.set<"value">(ruvia::json_value::parse(R"({"nested":null})"));
    EXPECT_EQ(model.get<"value">()->view(), std::string_view(R"({"nested":null})"));
    EXPECT_TRUE(!model.is_null<"value">() && !model.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":{"nested":null}})"));
}

TEST(Ruvia, model_nullable_dynamic_setter_keeps_value_null_and_input_presence_consistent) {
    for (const auto input : {"{}", R"({"value":null})"}) {
        auto model = ruvia::from_json<nullable_dynamic_value>(input);
        const bool present = model.is_present<"value">();
        model.reset<"value">();
        EXPECT_TRUE(!model.get<"value">() && !model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view("{}"));

        model.set<"value">(nullptr);
        EXPECT_TRUE(!model.get<"value">() && model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":null})"));

        model.set<"value">(ruvia::json_value::parse("[null,false,1]"));
        EXPECT_EQ(model.get<"value">()->view(), std::string_view("[null,false,1]"));
        EXPECT_TRUE(!model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":[null,false,1]})"));

        model.reset<"value">();
        EXPECT_TRUE(!model.get<"value">() && !model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view("{}"));

        model.set<"value">(ruvia::json_value::parse("true"));
        const auto null = ruvia::json_value::parse("null");
        model.set<"value">(null);
        EXPECT_TRUE(!model.get<"value">() && model.is_null<"value">());
        EXPECT_EQ(model.is_present<"value">(), present);
        EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":null})"));
    }
}

TEST(Ruvia, model_required_dynamic_setters_preserve_required_and_nullable_contracts) {
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_json<required_dynamic_value>("{}"); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_json<required_dynamic_value>(R"({"value":null})"); }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] { (void)ruvia::from_json<required_nullable_dynamic_value>("{}"); }));
    auto model = ruvia::from_json<required_dynamic_value>(R"({"value":1})");
    bool rejected = false;
    try {
        model.set<"value">(ruvia::json_value::parse("null"));
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    EXPECT_TRUE(rejected);
    EXPECT_EQ(model.get<"value">().view(), std::string_view("1"));
    EXPECT_TRUE(model.is_present<"value">() && !model.is_null<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":1})"));
    model.set<"value">(ruvia::json_value::parse("false"));
    EXPECT_EQ(model.get<"value">().view(), std::string_view("false"));
    EXPECT_EQ(std::string_view(ruvia::to_json(model)), std::string_view(R"({"value":false})"));

    required_nullable_dynamic_value response;
    ruvia::validator missing;
    ruvia::validate_model(response, missing);
    EXPECT_TRUE(!missing.ok());
    response.set<"value">(nullptr);
    EXPECT_TRUE(!response.get<"value">() && response.is_null<"value">());
    EXPECT_TRUE(!response.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(response)), std::string_view(R"({"value":null})"));
    ruvia::validator valid;
    ruvia::validate_model(response, valid);
    EXPECT_TRUE(valid.ok());
    auto parsed = ruvia::from_json<required_nullable_dynamic_value>(R"({"value":null})");
    parsed.set<"value">(ruvia::json_value::parse("42"));
    EXPECT_EQ(parsed.get<"value">()->view(), std::string_view("42"));
    EXPECT_TRUE(parsed.is_present<"value">() && !parsed.is_null<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(parsed)), std::string_view(R"({"value":42})"));
    parsed.set<"value">(ruvia::json_value::parse("null"));
    EXPECT_TRUE(!parsed.get<"value">() && parsed.is_null<"value">() && parsed.is_present<"value">());
    EXPECT_EQ(std::string_view(ruvia::to_json(parsed)), std::string_view(R"({"value":null})"));
}

TEST(Ruvia, model_nullable_response_emits_explicit_null_without_emitting_absence) {
    nullable_response response;
    response.set<"required">(nullptr);
    EXPECT_TRUE(response.is_null<"required">());
    EXPECT_EQ(std::string_view(ruvia::to_json(response)), std::string_view(R"({"required":null})"));
    response.set<"optional">(nullptr);
    auto token = ruvia::json_value::parse("null");
    response.set<"dynamic">(std::move(token));
    EXPECT_TRUE(response.is_null<"dynamic">());
    EXPECT_EQ(std::string_view(ruvia::to_json(response)),
        std::string_view(R"({"required":null,"optional":null,"dynamic":null})"));
    response.reset<"optional">();
    response.set<"required">("note");
    EXPECT_TRUE(!response.is_null<"required">());
    EXPECT_EQ(std::string_view(ruvia::to_json(response)),
        std::string_view(R"({"required":"note","dynamic":null})"));
    response.set_null<"optional">().reset<"optional">().set<"optional">("val");
    EXPECT_TRUE(response.has_value<"optional">());
    response.set_null<"optional">();
    EXPECT_TRUE(!response.has_value<"optional">() && response.is_null<"optional">());
    response.reset<"optional">();
    EXPECT_TRUE(!response.has_value<"optional">() && !response.is_null<"optional">());
}

TEST(Ruvia, model_dynamic_owned_tokens_and_presence_survive_rebinding_and_reclaim_memory) {
    ruvia::test::counting_memory_resource source;
    ruvia::test::counting_memory_resource destination;
    const std::string text(256, 'x');
    const std::string body = "{\"value\":{\"text\":\"" + text + "\"},\"object\":{\"text\":\"" + text + "\"}}";
    {
        owned_values retained({.resource_ = &destination});
        {
            auto input = body;
            auto parsed_value = ruvia::from_json<owned_values>(input, {.resource_ = &source});
            input.assign(input.size(), '?');
            owned_values moved(std::move(parsed_value));
            EXPECT_TRUE(moved.is_present<"value">() && !moved.is_present<"note">());
            retained = std::move(moved);
        }
        EXPECT_EQ(source.live_allocations(), std::size_t{0});
        EXPECT_TRUE(retained.is_present<"value">() && retained.is_present<"object">());
        EXPECT_TRUE(!retained.is_present<"note">() && retained.get<"note">().has_value());
        EXPECT_TRUE(retained.get<"value">()->get<ruvia::string>("text").value()->view() == text);
        const auto baseline = destination.live_allocations();
        for (int index = 0; index < 32; ++index) {
            {
                auto parsed_value = ruvia::from_json<owned_values>(body, {.resource_ = &destination});
                EXPECT_TRUE(parsed_value.is_present<"value">());
            }
            EXPECT_EQ(destination.live_allocations(), baseline);
            for (const auto& invalid : {body.substr(0, body.size() - 1) + ",\"value\":null}",
                     body.substr(0, body.size() - 1) + ",\"note\":42}", body.substr(0, body.size() - 1)}) {
                EXPECT_TRUE(ruvia::testing::throws_on(
                    [&] { (void)ruvia::from_json<owned_values>(invalid, {.resource_ = &destination}); }));
                EXPECT_EQ(destination.live_allocations(), baseline);
            }
            EXPECT_TRUE(retained.get<"object">()->get<ruvia::string>("text").value()->view() == text);
            EXPECT_EQ(destination.live_allocations(), baseline);
        }
        EXPECT_TRUE(destination.deallocation_count() > 0);
    }
    EXPECT_EQ(destination.live_allocations(), std::size_t{0});
}
