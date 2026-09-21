#include <array>
#include <memory_resource>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "ruvia/web/Model.h"
#include "ruvia/web/ModelForm.h"
#include "ruvia/web/ModelJson.h"
#include "ruvia/web/Validation.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

RUVIA_REQUEST_MODEL(RequiredValue, RUVIA_REQUIRED_FIELD(value, ruvia::String));
RUVIA_REQUEST_MODEL(OptionalValue, RUVIA_OPTIONAL_FIELD(value, ruvia::String));
RUVIA_REQUEST_MODEL(RequiredNullableValue,
    RUVIA_REQUIRED_FIELD(value, ruvia::String, RUVIA_NULLABLE));
RUVIA_REQUEST_MODEL(OptionalNullableValue,
    RUVIA_OPTIONAL_FIELD(value, ruvia::String, RUVIA_NULLABLE));
RUVIA_REQUEST_MODEL(DefaultValue,
    RUVIA_OPTIONAL_FIELD(value, ruvia::String, RUVIA_DEFAULT("fallback"), RUVIA_MIN(2, "too short")));
RUVIA_REQUEST_MODEL(NullableDefaultValue,
    RUVIA_OPTIONAL_FIELD(value, ruvia::String, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback"), RUVIA_MIN(2, "too short")));
RUVIA_REQUEST_MODEL(InvalidDefaultValue,
    RUVIA_OPTIONAL_FIELD(value, ruvia::UInt32, RUVIA_DEFAULT(3), RUVIA_MIN(5, "too small")));
RUVIA_REQUEST_MODEL(RequiredDefaultValue,
    RUVIA_REQUIRED_FIELD(value, ruvia::String, RUVIA_DEFAULT("fallback")));
RUVIA_REQUEST_MODEL(RequiredNullableDefaultValue,
    RUVIA_REQUIRED_FIELD(value, ruvia::String, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback")));
RUVIA_REQUEST_MODEL(NamedValue,
    RUVIA_OPTIONAL_FIELD_NAME("wire_value", value, ruvia::String, RUVIA_NULLABLE, RUVIA_DEFAULT("fallback")));
RUVIA_REQUEST_MODEL(PatchValue,
    RUVIA_OPTIONAL_FIELD(enabled, ruvia::Bool),
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String, RUVIA_NULLABLE));
RUVIA_REQUEST_MODEL(NestedValues,
    RUVIA_REQUIRED_FIELD(children, ruvia::Array<NullableDefaultValue>));
RUVIA_REQUEST_MODEL(NullableKinds,
    RUVIA_REQUIRED_FIELD(flag, ruvia::Bool, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(number, ruvia::UInt32, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(child, RequiredValue, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(items, ruvia::Array<ruvia::String>, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(boxes, ruvia::BoxedArray<RequiredValue>, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(payload, ruvia::JsonValue, RUVIA_NULLABLE),
    RUVIA_REQUIRED_FIELD(object, ruvia::JsonObject, RUVIA_NULLABLE));
RUVIA_REQUEST_MODEL(DynamicValue,
    RUVIA_OPTIONAL_FIELD(value, ruvia::JsonValue));
RUVIA_REQUEST_MODEL(DynamicObject,
    RUVIA_OPTIONAL_FIELD(value, ruvia::JsonObject));
RUVIA_REQUEST_MODEL(OwnedValues,
    RUVIA_OPTIONAL_FIELD(value, ruvia::JsonValue, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(object, ruvia::JsonObject, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(note, ruvia::String, RUVIA_DEFAULT("a default string long enough to allocate storage")));
RUVIA_RESPONSE_MODEL(NullableResponse,
    RUVIA_REQUIRED_FIELD(required, ruvia::String, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(optional, ruvia::String, RUVIA_NULLABLE),
    RUVIA_OPTIONAL_FIELD(dynamic, ruvia::JsonValue, RUVIA_NULLABLE));

class FailingMemoryResource final : public std::pmr::memory_resource {
public:
    explicit FailingMemoryResource(std::size_t remaining)
        : remaining_(remaining) {}
    [[nodiscard]] std::size_t liveAllocations() const {
        return memory_.liveAllocations();
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (remaining_ == 0) {
            throw std::bad_alloc();
        }
        --remaining_;
        return memory_.allocate(bytes, alignment);
    }
    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        memory_.deallocate(pointer, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
    std::size_t remaining_;
    ruvia::test::CountingMemoryResource memory_;
};

// Parsing remains separate from field-rule validation, as in middleware.
// State assertions use the public API even for partial/invalid input models.
template <typename Model>
void checkInput(ruvia::testing::TestContext& ruvia_ctx, std::string_view input,
    std::string_view code, bool present, bool null) {
    auto parsed = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<Model>(
        input, std::pmr::get_default_resource());
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK_EQ(parsed->template isPresent<"value">(), present);
    RUVIA_CHECK_EQ(parsed->template isNull<"value">(), null);
    ruvia::Validator validator;
    ruvia::detail::ModelValidationAccess::validateModel(*parsed, validator);
    if (code.empty()) {
        RUVIA_CHECK(validator.ok());
    } else {
        RUVIA_CHECK_EQ(validator.issues().size(), std::size_t{1});
        if (!validator.issues().empty()) {
            RUVIA_CHECK_EQ(validator.issues().front().code(), code);
        }
    }
}

template <typename Model>
void checkPresenceMatrix(ruvia::testing::TestContext& ruvia_ctx, bool required, bool nullable) {
    checkInput<Model>(ruvia_ctx, "{}", required ? "required" : "", false, false);
    checkInput<Model>(ruvia_ctx, R"({"value":null})", nullable ? "" : "invalid_type", true, nullable);
    checkInput<Model>(ruvia_ctx, R"({"value":"note"})", "", true, false);
    for (auto input : {R"({"value":42})", R"({"value":false})", R"({"value":[]})", R"({"value":{}})"}) {
        checkInput<Model>(ruvia_ctx, input, "invalid_type", true, false);
        RUVIA_CHECK(!ruvia::fromJson<Model>(input));
    }
    for (auto input : {R"({"value":null,"value":"note"})",
             R"({"value":"note","value":null})", R"({"value":null,"value":null})",
             R"({"value":42,"value":"note"})", R"({"value":"note","va\u006cue":"again"})"}) {
        checkInput<Model>(ruvia_ctx, input, "duplicate", true, false);
        RUVIA_CHECK(!ruvia::fromJson<Model>(input));
    }
    RUVIA_CHECK_EQ(ruvia::fromJson<Model>("{}").has_value(), !required);
    RUVIA_CHECK_EQ(ruvia::fromJson<Model>(R"({"value":null})").has_value(), nullable);
    RUVIA_CHECK(ruvia::fromJson<Model>(R"({"value":"note"})").has_value());
}

}  // namespace

RUVIA_TEST(model_field_presence_and_nullability_are_independent) {
    checkPresenceMatrix<RequiredValue>(ruvia_ctx, true, false);
    checkPresenceMatrix<OptionalValue>(ruvia_ctx, false, false);
    checkPresenceMatrix<RequiredNullableValue>(ruvia_ctx, true, true);
    checkPresenceMatrix<OptionalNullableValue>(ruvia_ctx, false, true);
    auto null = ruvia::fromJson<RequiredNullableValue>(R"({"value":null})");
    auto text = ruvia::fromJson<RequiredNullableValue>(R"({"value":"note"})");
    RUVIA_CHECK(null && !null->get<"value">());
    RUVIA_CHECK(text && text->get<"value">()->view() == "note");
}

RUVIA_TEST(model_patch_public_states_distinguish_omission_clear_and_assignment) {
    std::optional<std::string> remark{"original"};
    auto apply = [&](std::string_view json) {
        const auto patch = ruvia::fromJson<PatchValue>(json);
        RUVIA_CHECK(patch.has_value());
        if (!patch || !patch->isPresent<"remark">()) {
            return;
        }
        if (patch->isNull<"remark">()) {
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
    RUVIA_CHECK(!ruvia::fromJson<PatchValue>(R"({"enabled":null})"));
    const auto disabled = ruvia::fromJson<PatchValue>(R"({"enabled":false})");
    RUVIA_CHECK(disabled && disabled->isPresent<"enabled">() && !disabled->isNull<"enabled">());
    RUVIA_CHECK(disabled && !bool(*disabled->get<"enabled">()));
}

RUVIA_TEST(model_defaults_only_fill_optional_missing_values_and_are_validated) {
    checkInput<DefaultValue>(ruvia_ctx, "{}", "", false, false);
    checkInput<DefaultValue>(ruvia_ctx, R"({"value":null})", "invalid_type", true, false);
    checkInput<NullableDefaultValue>(ruvia_ctx, R"({"value":null})", "", true, true);
    checkInput<NullableDefaultValue>(ruvia_ctx, R"({"value":""})", "too_small", true, false);
    checkInput<InvalidDefaultValue>(ruvia_ctx, "{}", "too_small", false, false);
    checkInput<InvalidDefaultValue>(ruvia_ctx, R"({"value":8})", "", true, false);
    checkInput<RequiredDefaultValue>(ruvia_ctx, "{}", "required", false, false);
    checkInput<RequiredNullableDefaultValue>(ruvia_ctx, "{}", "required", false, false);
    checkInput<RequiredNullableDefaultValue>(ruvia_ctx, R"({"value":null})", "", true, true);
    RUVIA_CHECK(!ruvia::fromJson<RequiredDefaultValue>("{}"));
    RUVIA_CHECK(!ruvia::fromForm<RequiredDefaultValue>(""));
    auto missing = ruvia::fromJson<NullableDefaultValue>("{}");
    RUVIA_CHECK(missing && missing->get<"value">()->view() == "fallback");
    RUVIA_CHECK(missing && !missing->isPresent<"value">());
    auto form = ruvia::fromForm<DefaultValue>("");
    RUVIA_CHECK(form && !form->isPresent<"value">() && form->get<"value">()->view() == "fallback");
    auto invalid = ruvia::fromForm<InvalidDefaultValue>("");
    RUVIA_CHECK(invalid.has_value());
    if (invalid) {
        ruvia::Validator validator;
        ruvia::detail::ModelValidationAccess::validateModel(*invalid, validator);
        RUVIA_CHECK(!validator.ok());
    }
}

RUVIA_TEST(model_input_presence_survives_application_mutation_and_moves) {
    auto absent = ruvia::fromJson<NullableDefaultValue>("{}");
    auto present = ruvia::fromJson<NullableDefaultValue>(R"({"value":null})");
    RUVIA_CHECK(absent && present);
    if (!absent || !present) {
        return;
    }
    absent->set<"value">("application value");
    RUVIA_CHECK(!absent->isPresent<"value">());
    present->ensure<"value">().assignOwned(std::string_view("replacement"));
    RUVIA_CHECK(present->isPresent<"value">() && !present->isNull<"value">());
    present->reset<"value">();
    RUVIA_CHECK(present->isPresent<"value">() && !present->isNull<"value">());
    present->set<"value">(nullptr);
    NullableDefaultValue moved(std::move(*present));
    RUVIA_CHECK(moved.isPresent<"value">() && moved.isNull<"value">());
    RUVIA_CHECK(!present->isPresent<"value">() && !present->isNull<"value">());
    *absent = std::move(moved);
    RUVIA_CHECK(absent->isPresent<"value">() && absent->isNull<"value">());
    RUVIA_CHECK(!moved.isPresent<"value">());
}

RUVIA_TEST(model_named_form_and_nested_fields_keep_input_presence) {
    auto named = ruvia::fromJson<NamedValue>(R"({"wire_value":null})");
    RUVIA_CHECK(named && named->isPresent<"value">() && named->isNull<"value">());
    auto text = ruvia::fromForm<NamedValue>("wire_value=null");
    RUVIA_CHECK(text && text->isPresent<"value">() && !text->isNull<"value">());
    RUVIA_CHECK(text && text->get<"value">()->view() == "null");
    RUVIA_CHECK(!ruvia::fromForm<NamedValue>("wire_value=a&wire_value=b"));
    auto nested = ruvia::fromJson<NestedValues>(R"({"children":[{},{"value":null},{"value":"note"}]})");
    RUVIA_CHECK(nested.has_value());
    if (!nested) {
        return;
    }
    const auto& children = nested->get<"children">();
    RUVIA_CHECK(!children[0].isPresent<"value">() && children[0].get<"value">()->view() == "fallback");
    RUVIA_CHECK(children[1].isPresent<"value">() && children[1].isNull<"value">());
    RUVIA_CHECK(children[2].isPresent<"value">() && !children[2].isNull<"value">());
}

RUVIA_TEST(model_nullable_applies_to_scalar_container_nested_and_dynamic_types) {
    auto parsed = ruvia::fromJson<NullableKinds>(
        R"({"flag":null,"number":null,"child":null,"items":null,"boxes":null,"payload":null,"object":null})");
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK(parsed->isPresent<"flag">() && parsed->isNull<"flag">() && !parsed->get<"flag">());
    RUVIA_CHECK(parsed->isNull<"number">() && !parsed->get<"number">());
    RUVIA_CHECK(parsed->isNull<"child">() && !parsed->get<"child">());
    RUVIA_CHECK(parsed->isNull<"items">() && !parsed->get<"items">());
    RUVIA_CHECK(parsed->isNull<"boxes">() && !parsed->get<"boxes">());
    RUVIA_CHECK(parsed->isNull<"payload">() && !parsed->get<"payload">());
    RUVIA_CHECK(parsed->isNull<"object">() && !parsed->get<"object">());
    checkInput<DynamicValue>(ruvia_ctx, R"({"value":null})", "invalid_type", true, false);
    checkInput<DynamicObject>(ruvia_ctx, R"({"value":null})", "invalid_type", true, false);
    checkInput<DynamicObject>(ruvia_ctx, R"({"value":[]})", "invalid_type", true, false);
    checkInput<DynamicValue>(ruvia_ctx, R"({"value":{"nested":null}})", "", true, false);
    checkInput<DynamicValue>(ruvia_ctx, R"({"value":[null,false,1]})", "", true, false);
    checkInput<DynamicObject>(ruvia_ctx, R"({"value":{"nested":null}})", "", true, false);
}

RUVIA_TEST(model_nullable_response_emits_explicit_null_without_emitting_absence) {
    NullableResponse response;
    response.set<"required">(nullptr);
    RUVIA_CHECK(response.isNull<"required">());
    RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(response)), std::string_view(R"({"required":null})"));
    response.set<"optional">(nullptr);
    auto token = ruvia::JsonValue::parse("null");
    response.set<"dynamic">(std::move(*token));
    RUVIA_CHECK(response.isNull<"dynamic">());
    RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(response)),
        std::string_view(R"({"required":null,"optional":null,"dynamic":null})"));
    response.reset<"optional">();
    response.set<"required">("note");
    RUVIA_CHECK(!response.isNull<"required">());
    RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(response)),
        std::string_view(R"({"required":"note","dynamic":null})"));
}

RUVIA_TEST(model_dynamic_owned_tokens_and_presence_survive_rebinding_and_reclaim_memory) {
    ruvia::test::CountingMemoryResource source;
    ruvia::test::CountingMemoryResource destination;
    const std::string text(256, 'x');
    const std::string body = "{\"value\":{\"text\":\"" + text + "\"},\"object\":{\"text\":\"" + text + "\"}}";
    {
        OwnedValues retained({.resource = &destination});
        {
            auto input = body;
            auto parsed = ruvia::fromJson<OwnedValues>(input, {.resource = &source});
            RUVIA_CHECK(parsed.has_value());
            if (!parsed) {
                return;
            }
            input.assign(input.size(), '?');
            const auto beforeMove = source.allocationCount();
            OwnedValues moved(std::move(*parsed));
            RUVIA_CHECK_EQ(source.allocationCount(), beforeMove);
            RUVIA_CHECK(moved.isPresent<"value">() && !moved.isPresent<"note">());
            retained = std::move(moved);
        }
        RUVIA_CHECK_EQ(source.liveAllocations(), std::size_t{0});
        RUVIA_CHECK(retained.isPresent<"value">() && retained.isPresent<"object">());
        RUVIA_CHECK(!retained.isPresent<"note">() && retained.get<"note">().has_value());
        RUVIA_CHECK(retained.get<"value">()->get<ruvia::String>("text")->view() == text);
        const auto baseline = destination.liveAllocations();
        for (int index = 0; index < 32; ++index) {
            {
                auto parsed = ruvia::fromJson<OwnedValues>(body, {.resource = &destination});
                RUVIA_CHECK(parsed.has_value());
            }
            RUVIA_CHECK_EQ(destination.liveAllocations(), baseline);
            for (const auto& invalid : {body.substr(0, body.size() - 1) + ",\"value\":null}",
                     body.substr(0, body.size() - 1) + ",\"note\":42}", body.substr(0, body.size() - 1)}) {
                RUVIA_CHECK(!ruvia::fromJson<OwnedValues>(invalid, {.resource = &destination}));
                RUVIA_CHECK_EQ(destination.liveAllocations(), baseline);
            }
            RUVIA_CHECK(retained.get<"object">()->get<ruvia::String>("text")->view() == text);
            RUVIA_CHECK_EQ(destination.liveAllocations(), baseline);
        }
        RUVIA_CHECK(destination.deallocationCount() > 0);
    }
    RUVIA_CHECK_EQ(destination.liveAllocations(), std::size_t{0});
}

RUVIA_TEST(model_dynamic_allocation_failures_release_partial_values_and_defaults) {
    const std::string text(256, 'x');
    const std::string body = "{\"value\":{\"text\":\"" + text + "\"},\"object\":{\"text\":\"" + text + "\"}}";
    bool sawFailure = false;
    bool sawSuccess = false;
    for (std::size_t limit = 0; limit < 16; ++limit) {
        FailingMemoryResource resource(limit);
        try {
            auto parsed = ruvia::fromJson<OwnedValues>(body, {.resource = &resource});
            RUVIA_CHECK(parsed && parsed->isPresent<"value">() && !parsed->isPresent<"note">());
            sawSuccess = parsed.has_value();
        } catch (const std::bad_alloc&) {
            sawFailure = true;
        }
        RUVIA_CHECK_EQ(resource.liveAllocations(), std::size_t{0});
    }
    RUVIA_CHECK(sawFailure && sawSuccess);
}

RUVIA_TEST(model_dynamic_borrowed_tokens_stay_borrowed_until_explicit_ownership_transfer) {
    ruvia::test::CountingMemoryResource memory;
    const std::string text(256, 'b');
    std::string body = "{\"value\":{\"text\":\"" + text + "\"}}";
    {
        auto parsed = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<DynamicValue>(body, &memory);
        RUVIA_CHECK(parsed.has_value());
        if (!parsed) {
            return;
        }
        RUVIA_CHECK(parsed->isPresent<"value">());
        const auto borrowed = parsed->get<"value">()->view();
        RUVIA_CHECK(borrowed.data() == body.data() + std::string_view("{\"value\":").size());
        RUVIA_CHECK_EQ(memory.allocationCount(), std::size_t{0});
        auto moved = std::move(*parsed);
        RUVIA_CHECK(moved.get<"value">()->view().data() == borrowed.data());
        DynamicValue owned({.resource = &memory});
        owned = std::move(moved);
        body.assign(body.size(), '?');
        RUVIA_CHECK(owned.isPresent<"value">());
        RUVIA_CHECK(owned.get<"value">()->get<ruvia::String>("text")->view() == text);
        RUVIA_CHECK(memory.liveAllocations() > 0);
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), std::size_t{0});
}
