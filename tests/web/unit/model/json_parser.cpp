#include "model_field_fixture.h"

#include <memory_resource>
#include <string>
#include <string_view>

RUVIA_TEST(model_json_parser_dispatches_decoded_keys) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed = ruvia::JsonBody<AccessorSurfaceRequest>::parse(R"({"mess\u0061ge":"ready"})", &resource);
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK(parsed->message().has_value());
    if (!parsed->message()) {
        return;
    }
    RUVIA_CHECK_EQ(parsed->message()->view(), std::string_view("ready"));
}

RUVIA_TEST(model_json_parser_keeps_field_errors_separate_from_document_errors) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed = ruvia::JsonBody<NestedModelItem>::parse(R"({"id":"wrong","label":"still parsed"})", &resource);
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK(!parsed->id().has_value());
    RUVIA_CHECK(parsed->label().has_value());
    if (!parsed->label()) {
        return;
    }
    RUVIA_CHECK_EQ(parsed->label()->view(), std::string_view("still parsed"));
    RUVIA_CHECK(ruvia::detail::ModelValidationAccess::fieldState<"id">(*parsed) == ruvia::detail::ModelFieldState::kInvalidType);
}

RUVIA_TEST(model_json_parser_fully_validates_unknown_and_duplicate_values) {
    std::pmr::monotonic_buffer_resource resource;
    RUVIA_CHECK(!ruvia::JsonBody<AccessorSurfaceRequest>::parse(R"({"message":"ready","unknown":[1,]})", &resource).has_value());
    RUVIA_CHECK(!ruvia::JsonBody<AccessorSurfaceRequest>::parse(R"({"message":"first","message":{"broken":}})", &resource).has_value());

    const auto duplicate = ruvia::JsonBody<AccessorSurfaceRequest>::parse(R"({"message":"first","message":"second"})", &resource);
    RUVIA_CHECK(duplicate.has_value());
    if (!duplicate) {
        return;
    }
    RUVIA_CHECK(duplicate->message().has_value());
    if (!duplicate->message()) {
        return;
    }
    RUVIA_CHECK_EQ(duplicate->message()->view(), std::string_view("first"));
    RUVIA_CHECK(ruvia::detail::ModelValidationAccess::fieldState<"message">(*duplicate) == ruvia::detail::ModelFieldState::kDuplicate);
}

RUVIA_TEST(model_json_parser_enforces_depth_while_skipping_unknown_values) {
    std::string input = R"({"message":"ready","unknown":)";
    input.append(70, '[');
    input.append(70, ']');
    input.push_back('}');

    std::pmr::monotonic_buffer_resource resource;
    RUVIA_CHECK(!ruvia::JsonBody<AccessorSurfaceRequest>::parse(input, &resource).has_value());
}
