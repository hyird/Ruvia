#include "model_field_fixture.h"

RUVIA_TEST(model_public_parser_accepts_valid_strings_and_rejects_invalid_input) {
    std::pmr::monotonic_buffer_resource model_resource;
    const auto parsed_value = ruvia::from_json<accessor_surface_request>(
        R"({"message":"ready"})", {.resource_ = &model_resource});
    RUVIA_CHECK(parsed_value.has_value());
    if (parsed_value.has_value()) {
        const accessor_surface_request& model = *parsed_value;
        RUVIA_CHECK(model.get<"message">().has_value());
        if (model.get<"message">().has_value()) {
            RUVIA_CHECK_EQ(model.get<"message">()->view(), std::string_view("ready"));
        }
    }

    RUVIA_CHECK(!ruvia::from_json<accessor_surface_request>(
        R"({"message":42})", {.resource_ = std::pmr::get_default_resource()})
            .has_value());

    const auto malformed = ruvia::from_json<accessor_surface_request>(
        R"({"message":"incomplete")", {.resource_ = &model_resource});
    RUVIA_CHECK(!malformed.has_value());
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
