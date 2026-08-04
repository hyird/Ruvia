#include "model_field_fixture.h"

// Object lookup uses the last occurrence. Model binding keeps the first parsed
// value and marks the field duplicate so validation can reject ambiguity.

RUVIA_TEST(model_factory_materializes_before_publication) {
    std::pmr::monotonic_buffer_resource modelResource;
    const auto parsed = ruvia::JsonBody<AccessorSurfaceRequest>::parse(R"({"message":"ready"})", &modelResource);
    RUVIA_CHECK(parsed.has_value());
    if (parsed.has_value()) {
        const AccessorSurfaceRequest& model = *parsed;
        RUVIA_CHECK(model.message().has_value());
        if (model.message().has_value()) {
            RUVIA_CHECK_EQ(model.message()->view(), std::string_view("ready"));
            RUVIA_CHECK(model.message()->resource() == &modelResource);
        }
        RUVIA_CHECK(ruvia::detail::ModelValidationAccess::fieldState<"message">(model) == ruvia::detail::ModelFieldState::kParsed);
    }

    const auto invalidField = ruvia::JsonBody<AccessorSurfaceRequest>::parse(R"({"message":42})", std::pmr::get_default_resource());
    RUVIA_CHECK(invalidField.has_value());
    if (invalidField.has_value()) {
        RUVIA_CHECK(!invalidField->message().has_value());
        RUVIA_CHECK(ruvia::detail::ModelValidationAccess::fieldState<"message">(*invalidField) == ruvia::detail::ModelFieldState::kInvalidType);
    }

    const auto malformed = ruvia::JsonBody<AccessorSurfaceRequest>::parse(R"({"message":"incomplete")", &modelResource);
    RUVIA_CHECK(!malformed.has_value());

    AccessorSurfaceResponse response(&modelResource);
    RUVIA_CHECK(response.messageEnsure().resource() == &modelResource);
}

RUVIA_TEST(unified_model_parses_and_serializes_nested_arrays_and_optional_fields) {
    std::pmr::monotonic_buffer_resource resource;
    std::string input = R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})";
    const auto parsed = ruvia::fromJson<NestedModelEnvelope>(input, &resource);
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    input.assign(input.size(), 'x');

    RUVIA_CHECK(parsed->primary().has_value());
    RUVIA_CHECK(parsed->items().has_value());
    RUVIA_CHECK(parsed->tags().has_value());
    if (parsed->primary()) {
        RUVIA_CHECK(parsed->primary()->id().has_value());
        RUVIA_CHECK(!parsed->primary()->label().has_value());
    }
    if (parsed->items()) {
        RUVIA_CHECK_EQ(parsed->items()->size(), std::size_t{1});
        RUVIA_CHECK((*parsed->items())[0].label().has_value());
        if ((*parsed->items())[0].label()) {
            RUVIA_CHECK_EQ((*parsed->items())[0].label()->view(), std::string_view("two"));
        }
    }
    if (parsed->tags()) {
        RUVIA_CHECK_EQ(parsed->tags()->size(), std::size_t{2});
        RUVIA_CHECK_EQ((*parsed->tags())[0].view(), std::string_view("a"));
        RUVIA_CHECK_EQ((*parsed->tags())[1].view(), std::string_view("b"));
    }

    RUVIA_CHECK_EQ(std::string(ruvia::toJson(*parsed, &resource)), std::string(R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})"));
}

RUVIA_TEST(form_object_get_uses_last_match) {
    auto form = ruvia::FormObject::parse("name=first&other=x&name=second", std::pmr::get_default_resource());
    RUVIA_CHECK(form.has_value());

    const auto value = form->get<ruvia::String>("name");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(value->view(), std::string_view("second"));
}

RUVIA_TEST(form_object_get_uses_last_match_after_invalid_duplicate) {
    auto form = ruvia::FormObject::parse("age=nope&other=x&age=42", std::pmr::get_default_resource());
    RUVIA_CHECK(form.has_value());

    const auto value = form->get<ruvia::Int32>("age");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(static_cast<std::int32_t>(*value), 42);
}

RUVIA_TEST(json_object_get_uses_last_match) {
    auto json = ruvia::JsonObject::parse(R"({"name":"first","other":"x","name":"second"})", std::pmr::get_default_resource());
    RUVIA_CHECK(json.has_value());

    const auto value = json->get<ruvia::String>("name");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(value->view(), std::string_view("second"));
}
