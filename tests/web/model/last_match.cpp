#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "model_field_fixture.h"
#include "test_exceptions.h"

using namespace ruvia::test;

TEST(Ruvia, model_public_parser_accepts_valid_strings_and_rejects_invalid_input) {
    const auto model = ruvia::from_json<accessor_surface_request>(
        R"({"message":"ready"})");
    EXPECT_TRUE(model.get<"message">().has_value());
    if (model.get<"message">().has_value()) {
        EXPECT_EQ(model.get<"message">()->view(), std::string_view("ready"));
    }

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":42})");
    }));

    EXPECT_TRUE(ruvia::test::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"incomplete")");
    }));
}

TEST(Ruvia, request_and_response_models_support_nested_arrays_and_optional_fields) {
    std::string input(R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})");
    const auto parsed_value = ruvia::from_json<nested_model_envelope>(input);
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

    nested_response_envelope response;
    response.ensure<"primary">().set<"id">(ruvia::uint32{1});
    response.ensure<"items">()
        .emplace_back()
        .set<"id">(ruvia::uint32{2})
        .set<"label">("two");
    response.ensure<"tags">().emplace_back("a");
    response.ensure<"tags">().emplace_back("b");
    EXPECT_EQ(std::string_view(ruvia::to_json(response)),
        std::string_view(R"({"primary":{"id":1},"items":[{"id":2,"label":"two"}],"tags":["a","b"]})"));
}
