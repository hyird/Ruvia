#include <gtest/gtest.h>

#include <memory_resource>
#include <string>
#include <string_view>

#include "model_field_fixture.h"
#include "test_exceptions.h"

TEST(Ruvia, model_json_parser_dispatches_decoded_keys) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed_value = ruvia::from_json<accessor_surface_request>(
        R"({"mess\u0061ge":"ready"})", {.resource_ = &resource});
    EXPECT_TRUE(parsed_value.get<"message">().has_value());
    if (!parsed_value.get<"message">()) {
        return;
    }
    EXPECT_EQ(parsed_value.get<"message">()->view(), std::string_view("ready"));
}

TEST(Ruvia, model_public_parsers_own_string_fields) {
    std::pmr::monotonic_buffer_resource resource;
    auto json = ruvia::from_json<accessor_surface_request>(
        std::string(R"({"message":"json-owned"})"), {.resource_ = &resource});
    std::string form_input("message=form-owned");
    auto form = ruvia::from_form<accessor_surface_request>(form_input, {.resource_ = &resource});
    form_input.assign(form_input.size(), 'x');

    if (!json.get<"message">() || !form.get<"message">()) {
        return;
    }
    EXPECT_EQ(json.get<"message">()->view(), std::string_view("json-owned"));
    EXPECT_EQ(form.get<"message">()->view(), std::string_view("form-owned"));
}

TEST(Ruvia, model_form_parser_keeps_malformed_percent_sequences_literally) {
    for (const std::string_view invalid : {"%", "%G0", "%0G"}) {
        for (const auto& body : {
                 std::string("message=ready&unknown=").append(invalid),
                 std::string("unknown=").append(invalid) + "&message=ready",
                 std::string("message=ready&").append(invalid) + "=ignored"}) {
            const auto parsed = ruvia::from_form<accessor_surface_request>(body);
            EXPECT_TRUE(parsed.get<"message">().has_value());
        }
    }
    const auto literal = ruvia::from_form<accessor_surface_request>("message=100%");
    EXPECT_TRUE(literal.get<"message">().has_value());
    if (literal.get<"message">()) {
        EXPECT_EQ(literal.get<"message">()->view(), std::string_view("100%"));
    }
    const auto valid = ruvia::from_form<accessor_surface_request>(
        "message=hello+world&unknown=%00%26%3D%FF&%75nknown=value");
    EXPECT_TRUE(valid.get<"message">().has_value());
    if (valid.get<"message">()) {
        EXPECT_EQ(valid.get<"message">()->view(), std::string_view("hello world"));
    }
}

TEST(Ruvia, model_json_parser_rejects_invalid_field_types) {
    std::pmr::monotonic_buffer_resource resource;
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<nested_model_item>(
            R"({"id":"wrong","label":"still parsed"})", {.resource_ = &resource});
    }));
}

TEST(Ruvia, model_json_parser_fully_validates_unknown_and_duplicate_values) {
    std::pmr::monotonic_buffer_resource resource;
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"ready","unknown":[1,]})", {.resource_ = &resource});
    }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"ready","unknown":"\ud83d"})", {.resource_ = &resource});
    }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"first","message":{"broken":}})", {.resource_ = &resource});
    }));
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"first","message":"\ud83d"})", {.resource_ = &resource});
    }));

    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(
            R"({"message":"first","message":"second"})", {.resource_ = &resource});
    }));
}

TEST(Ruvia, model_json_parser_rejects_nested_structure_recursively) {
    std::pmr::monotonic_buffer_resource resource;
    constexpr auto body = R"({"primary":{},"items":[{"label":"missing id"}]})";
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<nested_model_envelope>(body, {.resource_ = &resource});
    }));
}

TEST(Ruvia, model_json_parser_enforces_depth_while_skipping_unknown_values) {
    std::string input = R"({"message":"ready","unknown":)";
    input.append(70, '[');
    input.append(70, ']');
    input.push_back('}');

    std::pmr::monotonic_buffer_resource resource;
    EXPECT_TRUE(ruvia::testing::throws_on([&] {
        (void)ruvia::from_json<accessor_surface_request>(input, {.resource_ = &resource});
    }));
}
