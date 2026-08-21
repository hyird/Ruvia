#include "model_field_fixture.h"

// Object lookup uses the last occurrence. Model binding keeps the first parsed
// value and marks the field duplicate so validation can reject ambiguity.

RUVIA_TEST(model_factory_materializes_before_publication) {
    std::pmr::monotonic_buffer_resource modelResource;
    const auto parsed = ruvia::fromJson<AccessorSurfaceRequest>(R"({"message":"ready"})", {.resource = &modelResource});
    RUVIA_CHECK(parsed.has_value());
    if (parsed.has_value()) {
        const AccessorSurfaceRequest& model = *parsed;
        RUVIA_CHECK(model.get<"message">().has_value());
        if (model.get<"message">().has_value()) {
            RUVIA_CHECK_EQ(model.get<"message">()->view(), std::string_view("ready"));
            RUVIA_CHECK(model.get<"message">()->resource() == &modelResource);
        }
        RUVIA_CHECK(ruvia::detail::ModelValidationAccess::fieldState<"message">(model) == ruvia::detail::ModelFieldState::kParsed);
    }

    RUVIA_CHECK(!ruvia::fromJson<AccessorSurfaceRequest>(R"({"message":42})", {.resource = std::pmr::get_default_resource()}).has_value());
    const auto invalidField = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<AccessorSurfaceRequest>(R"({"message":42})", std::pmr::get_default_resource());
    RUVIA_CHECK(invalidField.has_value());
    if (invalidField.has_value()) {
        RUVIA_CHECK(!invalidField->get<"message">().has_value());
        RUVIA_CHECK(ruvia::detail::ModelValidationAccess::fieldState<"message">(*invalidField) == ruvia::detail::ModelFieldState::kInvalidType);
    }

    const auto malformed = ruvia::fromJson<AccessorSurfaceRequest>(R"({"message":"incomplete")", {.resource = &modelResource});
    RUVIA_CHECK(!malformed.has_value());

    AccessorSurfaceResponse response({.resource = &modelResource});
    RUVIA_CHECK(response.ensure<"message">().resource() == &modelResource);
}

RUVIA_TEST(request_and_response_models_support_nested_arrays_and_optional_fields) {
    std::pmr::monotonic_buffer_resource resource;
    std::string input = R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})";
    const auto parsed = ruvia::fromJson<NestedModelEnvelope>(input, {.resource = &resource});
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    input.assign(input.size(), 'x');

    RUVIA_CHECK_EQ(std::uint32_t(parsed->get<"primary">().get<"id">()), std::uint32_t{1});
    RUVIA_CHECK_EQ(parsed->get<"items">().size(), std::size_t{1});
    RUVIA_CHECK(parsed->get<"tags">().has_value());
    RUVIA_CHECK(!parsed->get<"primary">().get<"label">().has_value());
    RUVIA_CHECK(parsed->get<"items">()[0].get<"label">().has_value());
    if (parsed->get<"items">()[0].get<"label">()) {
        RUVIA_CHECK_EQ(parsed->get<"items">()[0].get<"label">()->view(), std::string_view("two"));
    }
    if (parsed->get<"tags">()) {
        RUVIA_CHECK_EQ(parsed->get<"tags">()->size(), std::size_t{2});
        RUVIA_CHECK_EQ((*parsed->get<"tags">())[0].view(), std::string_view("a"));
        RUVIA_CHECK_EQ((*parsed->get<"tags">())[1].view(), std::string_view("b"));
    }

    NestedResponseEnvelope response({.resource = &resource});
    response.ensure<"primary">().set<"id">(ruvia::UInt32{1});
    response.ensure<"items">().emplace_back(ruvia::ModelOptions{.resource = &resource}).set<"id">(ruvia::UInt32{2}).set<"label">("two");
    response.ensure<"tags">().emplace_back("a", ruvia::ModelOptions{.resource = &resource});
    response.ensure<"tags">().emplace_back("b", ruvia::ModelOptions{.resource = &resource});
    RUVIA_CHECK_EQ(std::string(ruvia::toJson(response, {.resource = &resource})), std::string(R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})"));
}

RUVIA_TEST(form_object_get_uses_last_match) {
    auto form = ruvia::FormObject::parse("name=first&other=x&name=second", {.resource = std::pmr::get_default_resource()});
    RUVIA_CHECK(form.has_value());

    const auto value = form->get<ruvia::String>("name");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(value->view(), std::string_view("second"));
}

RUVIA_TEST(form_object_get_uses_last_match_after_invalid_duplicate) {
    auto form = ruvia::FormObject::parse("age=nope&other=x&age=42", {.resource = std::pmr::get_default_resource()});
    RUVIA_CHECK(form.has_value());

    const auto value = form->get<ruvia::Int32>("age");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(static_cast<std::int32_t>(*value), 42);
}

RUVIA_TEST(json_object_get_uses_last_match) {
    auto json = ruvia::JsonObject::parse(R"({"name":"first","other":"x","name":"second"})", {.resource = std::pmr::get_default_resource()});
    RUVIA_CHECK(json.has_value());

    const auto value = json->get<ruvia::String>("name");
    RUVIA_CHECK(value.has_value());
    RUVIA_CHECK_EQ(value->view(), std::string_view("second"));
}
