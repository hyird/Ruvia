#include <gtest/gtest.h>

#include "model_field_fixture.h"
#include "test_exceptions.h"

TEST(Ruvia, model_public_parser_accepts_valid_strings_and_rejects_invalid_input) {
    std::pmr::monotonic_buffer_resource model_resource;
    const auto model = ruvia::from_json<accessor_surface_request>(
        R"({"message":"ready"})", {.resource_ = &model_resource});
    EXPECT_TRUE(model.get<"message">().has_value());
    if (model.get<"message">().has_value()) {
        EXPECT_EQ(model.get<"message">()->view(), std::string_view("ready"));
    }

    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":42})", {.resource_ = std::pmr::get_default_resource()});
    }));

    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"incomplete")", {.resource_ = &model_resource});
    }));
}

TEST(Ruvia, request_and_response_models_support_nested_arrays_and_optional_fields) {
    std::pmr::monotonic_buffer_resource resource;
    std::string input = R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})";
    const auto parsed_value = ruvia::from_json<nested_model_envelope>(input, {.resource_ = &resource});
    input.assign(input.size(), 'x');

    EXPECT_EQ(std::uint32_t(parsed_value.get<"primary">().get<"id">()), std::uint32_t{1});
    EXPECT_EQ(parsed_value.get<"items">().size(), std::size_t{1});
    EXPECT_TRUE(parsed_value.get<"tags">().has_value());
    EXPECT_TRUE(!parsed_value.get<"primary">().get<"label">().has_value());
    EXPECT_TRUE(parsed_value.get<"items">()[0].get<"label">().has_value());
    if (parsed_value.get<"items">()[0].get<"label">()) {
        EXPECT_EQ(parsed_value.get<"items">()[0].get<"label">()->view(), std::string_view("two"));
    }
    if (parsed_value.get<"tags">()) {
        EXPECT_EQ(parsed_value.get<"tags">()->size(), std::size_t{2});
        EXPECT_EQ((*parsed_value.get<"tags">())[0].view(), std::string_view("a"));
        EXPECT_EQ((*parsed_value.get<"tags">())[1].view(), std::string_view("b"));
    }

    nested_response_envelope response({.resource_ = &resource});
    response.ensure<"primary">().set<"id">(ruvia::uint32{1});
    response.ensure<"items">()
        .emplace_back(ruvia::model_options{.resource_ = &resource})
        .set<"id">(ruvia::uint32{2})
        .set<"label">("two");
    response.ensure<"tags">().emplace_back("a", ruvia::model_options{.resource_ = &resource});
    response.ensure<"tags">().emplace_back("b", ruvia::model_options{.resource_ = &resource});
    EXPECT_EQ(std::string(ruvia::to_json(response, {.resource_ = &resource})),
        std::string(R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})"));
}
