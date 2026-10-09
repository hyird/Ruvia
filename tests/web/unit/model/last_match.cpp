#include "model_field_fixture.h"

// Object lookup uses the last occurrence. Model binding keeps the first parsed
// value and marks the field duplicate so validation can reject ambiguity.

RUVIA_TEST(model_factory_materializes_before_publication) {
    std::pmr::monotonic_buffer_resource model_resource;
    const auto parsed_value = ruvia::from_json<accessor_surface_request>(
        R"({"message":"ready"})", {.resource_ = &model_resource});
    RUVIA_CHECK(parsed_value.has_value());
    if (parsed_value.has_value()) {
        const accessor_surface_request& model = *parsed_value;
        RUVIA_CHECK(model.get<"message">().has_value());
        if (model.get<"message">().has_value()) {
            RUVIA_CHECK_EQ(model.get<"message">()->view(), std::string_view("ready"));
            RUVIA_CHECK(model.get<"message">()->resource() == &model_resource);
        }
        RUVIA_CHECK(ruvia::detail::model_validation_access::field_state<"message">(model) ==
                    ruvia::detail::model_field_state::parsed);
    }

    RUVIA_CHECK(!ruvia::from_json<accessor_surface_request>(
        R"({"message":42})", {.resource_ = std::pmr::get_default_resource()})
            .has_value());
    const auto invalid_field =
        ruvia::detail::model_parse_access::parse_json_borrowed_partial<accessor_surface_request>(
            R"({"message":42})", std::pmr::get_default_resource());
    RUVIA_CHECK(invalid_field.has_value());
    if (invalid_field.has_value()) {
        RUVIA_CHECK(!invalid_field->get<"message">().has_value());
        RUVIA_CHECK(ruvia::detail::model_validation_access::field_state<"message">(*invalid_field) ==
                    ruvia::detail::model_field_state::invalid_type);
    }

    const auto malformed = ruvia::from_json<accessor_surface_request>(
        R"({"message":"incomplete")", {.resource_ = &model_resource});
    RUVIA_CHECK(!malformed.has_value());

    accessor_surface_response response({.resource_ = &model_resource});
    RUVIA_CHECK(response.ensure<"message">().resource() == &model_resource);
}

RUVIA_MODEL(two_field_request, RUVIA_OPTIONAL_FIELD(message, ruvia::string),
    RUVIA_OPTIONAL_FIELD(tag, ruvia::string));

RUVIA_TEST(json_invalid_type_does_not_rescan_before_the_next_field) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed_value = ruvia::detail::model_parse_access::parse_json_borrowed_partial<two_field_request>(
        R"({"message":42,"tag":"ok"})", &resource);
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    RUVIA_CHECK(ruvia::detail::model_validation_access::field_state<"message">(*parsed_value) ==
                ruvia::detail::model_field_state::invalid_type);
    RUVIA_CHECK(parsed_value->get<"tag">().has_value());
    RUVIA_CHECK_EQ(parsed_value->get<"tag">()->view(), std::string_view("ok"));
}

RUVIA_TEST(request_and_response_models_support_nested_arrays_and_optional_fields) {
    std::pmr::monotonic_buffer_resource resource;
    std::string input = R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})";
    const auto parsed_value = ruvia::from_json<nested_model_envelope>(input, {.resource_ = &resource});
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    input.assign(input.size(), 'x');

    RUVIA_CHECK_EQ(std::uint32_t(parsed_value->get<"primary">().get<"id">()), std::uint32_t{1});
    RUVIA_CHECK_EQ(parsed_value->get<"items">().size(), std::size_t{1});
    RUVIA_CHECK(parsed_value->get<"tags">().has_value());
    RUVIA_CHECK(!parsed_value->get<"primary">().get<"label">().has_value());
    RUVIA_CHECK(parsed_value->get<"items">()[0].get<"label">().has_value());
    if (parsed_value->get<"items">()[0].get<"label">()) {
        RUVIA_CHECK_EQ(parsed_value->get<"items">()[0].get<"label">()->view(), std::string_view("two"));
    }
    if (parsed_value->get<"tags">()) {
        RUVIA_CHECK_EQ(parsed_value->get<"tags">()->size(), std::size_t{2});
        RUVIA_CHECK_EQ((*parsed_value->get<"tags">())[0].view(), std::string_view("a"));
        RUVIA_CHECK_EQ((*parsed_value->get<"tags">())[1].view(), std::string_view("b"));
    }

    nested_response_envelope response({.resource_ = &resource});
    response.ensure<"primary">().set<"id">(ruvia::uint32{1});
    response.ensure<"items">()
        .emplace_back(ruvia::model_options{.resource_ = &resource})
        .set<"id">(ruvia::uint32{2})
        .set<"label">("two");
    response.ensure<"tags">().emplace_back("a", ruvia::model_options{.resource_ = &resource});
    response.ensure<"tags">().emplace_back("b", ruvia::model_options{.resource_ = &resource});
    RUVIA_CHECK_EQ(std::string(ruvia::to_json(response, {.resource_ = &resource})),
        std::string(R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})"));
}
