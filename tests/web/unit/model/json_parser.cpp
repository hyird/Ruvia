#include "model_field_fixture.h"

#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/Validation.h"

RUVIA_TEST(model_json_parser_dispatches_decoded_keys) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed = ruvia::fromJson<AccessorSurfaceRequest>(R"({"mess\u0061ge":"ready"})", {.resource = &resource});
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK(parsed->get<"message">().has_value());
    if (!parsed->get<"message">()) {
        return;
    }
    RUVIA_CHECK_EQ(parsed->get<"message">()->view(), std::string_view("ready"));
}

RUVIA_TEST(model_public_parsers_own_string_fields) {
    std::pmr::monotonic_buffer_resource resource;
    auto json = ruvia::fromJson<AccessorSurfaceRequest>(std::string(R"({"message":"json-owned"})"), {.resource = &resource});
    std::string formInput("message=form-owned");
    auto form = ruvia::fromForm<AccessorSurfaceRequest>(formInput, {.resource = &resource});
    formInput.assign(formInput.size(), 'x');

    RUVIA_CHECK(json.has_value());
    RUVIA_CHECK(form.has_value());
    if (!json || !form || !json->get<"message">() || !form->get<"message">()) {
        return;
    }
    RUVIA_CHECK_EQ(json->get<"message">()->view(), std::string_view("json-owned"));
    RUVIA_CHECK_EQ(form->get<"message">()->view(), std::string_view("form-owned"));
}

RUVIA_TEST(model_internal_request_parsers_keep_literal_strings_borrowed) {
    std::pmr::monotonic_buffer_resource resource;
    std::string jsonInput(R"({"message":"json-borrowed"})");
    std::string formInput("message=form-borrowed");
    auto json = ruvia::detail::ModelParseAccess::parseJsonBorrowed<AccessorSurfaceRequest>(jsonInput, &resource);
    auto form = ruvia::detail::ModelParseAccess::parseFormBorrowed<AccessorSurfaceRequest>(formInput, &resource);

    RUVIA_CHECK(json.has_value());
    RUVIA_CHECK(form.has_value());
    if (!json || !form || !json->get<"message">() || !form->get<"message">()) {
        return;
    }
    RUVIA_CHECK(json->get<"message">()->data() == jsonInput.data() + 12);
    RUVIA_CHECK(form->get<"message">()->data() == formInput.data() + 8);
}

RUVIA_TEST(model_json_parser_keeps_field_errors_separate_from_document_errors) {
    std::pmr::monotonic_buffer_resource resource;
    RUVIA_CHECK(!ruvia::fromJson<NestedModelItem>(R"({"id":"wrong","label":"still parsed"})", {.resource = &resource}).has_value());
    const auto parsed = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<NestedModelItem>(R"({"id":"wrong","label":"still parsed"})", &resource);
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK(parsed->get<"label">().has_value());
    if (!parsed->get<"label">()) {
        return;
    }
    RUVIA_CHECK_EQ(parsed->get<"label">()->view(), std::string_view("still parsed"));
    RUVIA_CHECK(ruvia::detail::ModelValidationAccess::fieldState<"id">(*parsed) == ruvia::detail::ModelFieldState::kInvalidType);
}

RUVIA_TEST(model_json_parser_fully_validates_unknown_and_duplicate_values) {
    std::pmr::monotonic_buffer_resource resource;
    RUVIA_CHECK(!ruvia::fromJson<AccessorSurfaceRequest>(R"({"message":"ready","unknown":[1,]})", {.resource = &resource}).has_value());
    RUVIA_CHECK(!ruvia::fromJson<AccessorSurfaceRequest>(R"({"message":"ready","unknown":"\ud83d"})", {.resource = &resource}).has_value());
    RUVIA_CHECK(!ruvia::fromJson<AccessorSurfaceRequest>(R"({"message":"first","message":{"broken":}})", {.resource = &resource}).has_value());
    RUVIA_CHECK(!ruvia::fromJson<AccessorSurfaceRequest>(R"({"message":"first","message":"\ud83d"})", {.resource = &resource}).has_value());

    RUVIA_CHECK(!ruvia::fromJson<AccessorSurfaceRequest>(R"({"message":"first","message":"second"})", {.resource = &resource}).has_value());
    const auto duplicate = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<AccessorSurfaceRequest>(R"({"message":"first","message":"second"})", &resource);
    RUVIA_CHECK(duplicate.has_value());
    if (!duplicate) {
        return;
    }
    RUVIA_CHECK(duplicate->get<"message">().has_value());
    if (!duplicate->get<"message">()) {
        return;
    }
    RUVIA_CHECK_EQ(duplicate->get<"message">()->view(), std::string_view("first"));
    RUVIA_CHECK(ruvia::detail::ModelValidationAccess::fieldState<"message">(*duplicate) == ruvia::detail::ModelFieldState::kDuplicate);
}

RUVIA_TEST(model_json_parser_rejects_nested_structure_recursively) {
    std::pmr::monotonic_buffer_resource resource;
    constexpr auto body = R"({"primary":{},"items":[{"label":"missing id"}]})";
    RUVIA_CHECK(!ruvia::fromJson<NestedModelEnvelope>(body, {.resource = &resource}).has_value());

    const auto partial = ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<NestedModelEnvelope>(body, &resource);
    RUVIA_CHECK(partial.has_value());
    if (!partial) {
        return;
    }

    ruvia::Validator validator({.resource = &resource});
    ruvia::detail::ModelValidationAccess::validateStructure(*partial, {}, validator);
    RUVIA_CHECK_EQ(validator.issues().size(), std::size_t{2});
    RUVIA_CHECK_EQ(validator.issues()[0].field(), std::string_view("primary.id"));
    RUVIA_CHECK_EQ(validator.issues()[1].field(), std::string_view("items[0].id"));
}

RUVIA_TEST(model_json_parser_enforces_depth_while_skipping_unknown_values) {
    std::string input = R"({"message":"ready","unknown":)";
    input.append(70, '[');
    input.append(70, ']');
    input.push_back('}');

    std::pmr::monotonic_buffer_resource resource;
    RUVIA_CHECK(!ruvia::fromJson<AccessorSurfaceRequest>(input, {.resource = &resource}).has_value());
}
