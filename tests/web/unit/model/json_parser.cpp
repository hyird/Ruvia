#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/validation.h"

#include "model_field_fixture.h"

RUVIA_TEST(model_json_parser_dispatches_decoded_keys) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed_value = ruvia::from_json<accessor_surface_request>(
        R"({"mess\u0061ge":"ready"})", {.resource_ = &resource});
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    RUVIA_CHECK(parsed_value->get<"message">().has_value());
    if (!parsed_value->get<"message">()) {
        return;
    }
    RUVIA_CHECK_EQ(parsed_value->get<"message">()->view(), std::string_view("ready"));
}

RUVIA_TEST(model_public_parsers_own_string_fields) {
    std::pmr::monotonic_buffer_resource resource;
    auto json = ruvia::from_json<accessor_surface_request>(
        std::string(R"({"message":"json-owned"})"), {.resource_ = &resource});
    std::string form_input("message=form-owned");
    auto form = ruvia::from_form<accessor_surface_request>(form_input, {.resource_ = &resource});
    form_input.assign(form_input.size(), 'x');

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
    std::string json_input(R"({"message":"json-borrowed"})");
    std::string form_input("message=form-borrowed");
    auto json = ruvia::detail::model_parse_access::parse_json_borrowed<accessor_surface_request>(
        json_input, &resource);
    auto form = ruvia::detail::model_parse_access::parse_form_borrowed<accessor_surface_request>(
        form_input, &resource);

    RUVIA_CHECK(json.has_value());
    RUVIA_CHECK(form.has_value());
    if (!json || !form || !json->get<"message">() || !form->get<"message">()) {
        return;
    }
    RUVIA_CHECK(json->get<"message">()->data() == json_input.data() + 12);
    RUVIA_CHECK(form->get<"message">()->data() == form_input.data() + 8);
}

RUVIA_TEST(model_json_parser_keeps_field_errors_separate_from_document_errors) {
    std::pmr::monotonic_buffer_resource resource;
    RUVIA_CHECK(!ruvia::from_json<nested_model_item>(
        R"({"id":"wrong","label":"still parsed"})", {.resource_ = &resource})
            .has_value());
    const auto parsed_value = ruvia::detail::model_parse_access::parse_json_borrowed_partial<nested_model_item>(
        R"({"id":"wrong","label":"still parsed"})", &resource);
    RUVIA_CHECK(parsed_value.has_value());
    if (!parsed_value) {
        return;
    }
    RUVIA_CHECK(parsed_value->get<"label">().has_value());
    if (!parsed_value->get<"label">()) {
        return;
    }
    RUVIA_CHECK_EQ(parsed_value->get<"label">()->view(), std::string_view("still parsed"));
    RUVIA_CHECK(ruvia::detail::model_validation_access::field_state<"id">(*parsed_value) ==
                ruvia::detail::model_field_state::invalid_type);
}

RUVIA_TEST(model_json_parser_fully_validates_unknown_and_duplicate_values) {
    std::pmr::monotonic_buffer_resource resource;
    RUVIA_CHECK(!ruvia::from_json<accessor_surface_request>(
        R"({"message":"ready","unknown":[1,]})", {.resource_ = &resource})
            .has_value());
    RUVIA_CHECK(!ruvia::from_json<accessor_surface_request>(
        R"({"message":"ready","unknown":"\ud83d"})", {.resource_ = &resource})
            .has_value());
    RUVIA_CHECK(!ruvia::from_json<accessor_surface_request>(
        R"({"message":"first","message":{"broken":}})", {.resource_ = &resource})
            .has_value());
    RUVIA_CHECK(!ruvia::from_json<accessor_surface_request>(
        R"({"message":"first","message":"\ud83d"})", {.resource_ = &resource})
            .has_value());

    RUVIA_CHECK(!ruvia::from_json<accessor_surface_request>(
        R"({"message":"first","message":"second"})", {.resource_ = &resource})
            .has_value());
    const auto duplicate =
        ruvia::detail::model_parse_access::parse_json_borrowed_partial<accessor_surface_request>(
            R"({"message":"first","message":"second"})", &resource);
    RUVIA_CHECK(duplicate.has_value());
    if (!duplicate) {
        return;
    }
    RUVIA_CHECK(duplicate->get<"message">().has_value());
    if (!duplicate->get<"message">()) {
        return;
    }
    RUVIA_CHECK_EQ(duplicate->get<"message">()->view(), std::string_view("first"));
    RUVIA_CHECK(ruvia::detail::model_validation_access::field_state<"message">(*duplicate) ==
                ruvia::detail::model_field_state::duplicate);
}

RUVIA_TEST(model_json_parser_rejects_nested_structure_recursively) {
    std::pmr::monotonic_buffer_resource resource;
    constexpr auto body = R"({"primary":{},"items":[{"label":"missing id"}]})";
    RUVIA_CHECK(!ruvia::from_json<nested_model_envelope>(body, {.resource_ = &resource}).has_value());

    const auto partial =
        ruvia::detail::model_parse_access::parse_json_borrowed_partial<nested_model_envelope>(
            body, &resource);
    RUVIA_CHECK(partial.has_value());
    if (!partial) {
        return;
    }

    ruvia::validator validator_value({.resource_ = &resource});
    ruvia::detail::model_validation_access::validate_structure(*partial, {}, validator_value);
    RUVIA_CHECK_EQ(validator_value.issues().size(), std::size_t{2});
    RUVIA_CHECK_EQ(validator_value.issues()[0].field(), std::string_view("primary.id"));
    RUVIA_CHECK_EQ(validator_value.issues()[1].field(), std::string_view("items[0].id"));
}

RUVIA_TEST(model_json_parser_enforces_depth_while_skipping_unknown_values) {
    std::string input = R"({"message":"ready","unknown":)";
    input.append(70, '[');
    input.append(70, ']');
    input.push_back('}');

    std::pmr::monotonic_buffer_resource resource;
    RUVIA_CHECK(
        !ruvia::from_json<accessor_surface_request>(input, {.resource_ = &resource}).has_value());
}
