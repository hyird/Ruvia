#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/Model.h"
#include "ruvia/web/ModelJson.h"
#include "ruvia/web/Validation.h"

#include "test_harness.h"

namespace {

RUVIA_REQUEST_MODEL(RemarkRequest, RUVIA_OPTIONAL_FIELD(remark, ruvia::String));

RUVIA_REQUEST_MODEL(RemarkWithDefaultRequest,
    RUVIA_OPTIONAL_FIELD(remark, ruvia::String, RUVIA_DEFAULT("fallback")));

RUVIA_REQUEST_MODEL(RequiredRemarkRequest, RUVIA_REQUIRED_FIELD(remark, ruvia::String));

RUVIA_REQUEST_MODEL(JsonBagRequest, RUVIA_OPTIONAL_FIELD(payload, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(object, ruvia::JsonObject),
    RUVIA_OPTIONAL_FIELD(items, ruvia::Array<ruvia::JsonValue>));

RUVIA_RESPONSE_MODEL(JsonBagResponse, RUVIA_OPTIONAL_FIELD(payload, ruvia::JsonValue),
    RUVIA_OPTIONAL_FIELD(object, ruvia::JsonObject));

}  // namespace

RUVIA_TEST(nullable_optional_string_accepts_json_null) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed =
        ruvia::fromJson<RemarkRequest>(R"({"remark":null})", {.resource = &resource});
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK(!parsed->get<"remark">().has_value());
    RUVIA_CHECK(ruvia::detail::ModelValidationAccess::fieldState<"remark">(*parsed) ==
                ruvia::detail::ModelFieldState::kNull);

    ruvia::Validator validator;
    ruvia::detail::ModelValidationAccess::validateModel(*parsed, validator);
    RUVIA_CHECK(validator.ok());
}

RUVIA_TEST(nullable_optional_string_still_accepts_text) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed =
        ruvia::fromJson<RemarkRequest>(R"({"remark":"note"})", {.resource = &resource});
    RUVIA_CHECK(parsed.has_value());
    if (!parsed || !parsed->get<"remark">()) {
        return;
    }
    RUVIA_CHECK_EQ(parsed->get<"remark">()->view(), std::string_view("note"));
}

RUVIA_TEST(nullable_null_does_not_apply_default) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed = ruvia::fromJson<RemarkWithDefaultRequest>(
        R"({"remark":null})", {.resource = &resource});
    RUVIA_CHECK(parsed.has_value());
    if (!parsed) {
        return;
    }
    RUVIA_CHECK(!parsed->get<"remark">().has_value());
    RUVIA_CHECK(ruvia::detail::ModelValidationAccess::fieldState<"remark">(*parsed) ==
                ruvia::detail::ModelFieldState::kNull);

    const auto missing =
        ruvia::fromJson<RemarkWithDefaultRequest>("{}", {.resource = &resource});
    RUVIA_CHECK(missing.has_value());
    if (!missing || !missing->get<"remark">()) {
        return;
    }
    RUVIA_CHECK_EQ(missing->get<"remark">()->view(), std::string_view("fallback"));
}

RUVIA_TEST(required_string_rejects_json_null) {
    std::pmr::monotonic_buffer_resource resource;
    RUVIA_CHECK(!ruvia::fromJson<RequiredRemarkRequest>(R"({"remark":null})", {.resource = &resource})
            .has_value());

    const auto partial =
        ruvia::detail::ModelParseAccess::parseJsonBorrowedPartial<RequiredRemarkRequest>(
            R"({"remark":null})", &resource);
    RUVIA_CHECK(partial.has_value());
    if (!partial) {
        return;
    }
    RUVIA_CHECK(ruvia::detail::ModelValidationAccess::fieldState<"remark">(*partial) ==
                ruvia::detail::ModelFieldState::kInvalidType);
}

RUVIA_TEST(json_value_model_fields_accept_any_json_token) {
    std::pmr::monotonic_buffer_resource resource;
    const auto parsed = ruvia::fromJson<JsonBagRequest>(
        R"({"payload":[1,{"k":null}],"object":{"a":true},"items":[null,"x",{"z":2}]})",
        {.resource = &resource});
    RUVIA_CHECK(parsed.has_value());
    if (!parsed || !parsed->get<"payload">() || !parsed->get<"object">() ||
        !parsed->get<"items">()) {
        return;
    }
    RUVIA_CHECK(parsed->get<"payload">()->isArray());
    RUVIA_CHECK_EQ(parsed->get<"payload">()->view(), std::string_view(R"([1,{"k":null}])"));
    RUVIA_CHECK_EQ(parsed->get<"object">()->view(), std::string_view(R"({"a":true})"));
    const auto& items = *parsed->get<"items">();
    RUVIA_CHECK_EQ(items.size(), std::size_t{3});
    RUVIA_CHECK(items[0].isNull());
    RUVIA_CHECK(items[1].isString());
}

RUVIA_TEST(json_object_model_field_rejects_non_objects) {
    std::pmr::monotonic_buffer_resource resource;
    RUVIA_CHECK(!ruvia::fromJson<JsonBagRequest>(R"({"object":[1]})", {.resource = &resource})
            .has_value());

    const auto nullObject =
        ruvia::fromJson<JsonBagRequest>(R"({"object":null})", {.resource = &resource});
    RUVIA_CHECK(nullObject.has_value());
    if (nullObject) {
        RUVIA_CHECK(!nullObject->get<"object">().has_value());
        RUVIA_CHECK(ruvia::detail::ModelValidationAccess::fieldState<"object">(*nullObject) ==
                    ruvia::detail::ModelFieldState::kNull);
    }
}

RUVIA_TEST(json_value_model_fields_own_tokens_for_from_json) {
    std::pmr::monotonic_buffer_resource resource;
    auto parsed = ruvia::fromJson<JsonBagRequest>(
        std::string(R"({"payload":{"keep":true}})"), {.resource = &resource});
    RUVIA_CHECK(parsed.has_value());
    if (!parsed || !parsed->get<"payload">()) {
        return;
    }
    RUVIA_CHECK(parsed->get<"payload">()->isObject());
    RUVIA_CHECK_EQ(parsed->get<"payload">()->view(), std::string_view(R"({"keep":true})"));
}

RUVIA_TEST(json_value_response_fields_write_raw_tokens) {
    std::pmr::monotonic_buffer_resource resource;
    auto parsed = ruvia::fromJson<JsonBagRequest>(
        R"({"payload":[1,2],"object":{"a":1}})", {.resource = &resource});
    RUVIA_CHECK(parsed.has_value());
    if (!parsed || !parsed->get<"payload">() || !parsed->get<"object">()) {
        return;
    }

    JsonBagResponse response({.resource = &resource});
    response.set<"payload">(std::move(parsed->ensure<"payload">()));
    response.set<"object">(std::move(parsed->ensure<"object">()));
    RUVIA_CHECK_EQ(std::string_view(ruvia::toJson(response, {.resource = &resource})),
        std::string_view(R"({"payload":[1,2],"object":{"a":1}})"));
}
