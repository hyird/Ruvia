#pragma once

#include "test_harness.h"

#include <concepts>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/http/HttpRequest.h"
#include "ruvia/web/Model.h"

namespace model_field_test {

template <typename T>
concept HasCookiesAccessor = requires(const T& request) { request.cookies(); };

template <typename T>
concept HasQueryListAccessor = requires(const T& request) { request.query(); };

template <typename T>
concept HasQueriesVectorAccessor =
    requires(const T& request) { request.queries(std::string_view{}); };

template <typename T>
concept ParsesAnyRvalueOwningString = requires(std::string&& body) {
    T::parse(
        std::move(body), ruvia::ModelParseOptions{.resource = std::pmr::get_default_resource()});
} || requires(const std::string&& body) {
    T::parse(
        std::move(body), ruvia::ModelParseOptions{.resource = std::pmr::get_default_resource()});
};

template <typename T>
concept ParsesLvalueOwningString = requires(const std::string& body) {
    T::parse(body, ruvia::ModelParseOptions{.resource = std::pmr::get_default_resource()});
};

static_assert(!HasCookiesAccessor<ruvia::HttpRequest>);
static_assert(!HasQueryListAccessor<ruvia::HttpRequest>);
static_assert(!HasQueriesVectorAccessor<ruvia::HttpRequest>);
static_assert(!ParsesAnyRvalueOwningString<ruvia::JsonValue>);
static_assert(!ParsesAnyRvalueOwningString<ruvia::JsonObject>);
static_assert(!ParsesAnyRvalueOwningString<ruvia::FormObject>);
static_assert(ParsesLvalueOwningString<ruvia::JsonValue>);
static_assert(ParsesLvalueOwningString<ruvia::JsonObject>);
static_assert(ParsesLvalueOwningString<ruvia::FormObject>);

RUVIA_REQUEST_MODEL(AccessorSurfaceRequest, RUVIA_OPTIONAL_FIELD(message, ruvia::String));

RUVIA_RESPONSE_MODEL(AccessorSurfaceResponse, RUVIA_OPTIONAL_FIELD(message, ruvia::String));

RUVIA_REQUEST_MODEL(NestedModelItem, RUVIA_REQUIRED_FIELD(id, ruvia::UInt32),
    RUVIA_OPTIONAL_FIELD(label, ruvia::String));

RUVIA_REQUEST_MODEL(NestedModelEnvelope, RUVIA_REQUIRED_FIELD(primary, NestedModelItem),
    RUVIA_REQUIRED_FIELD(items, ruvia::Array<NestedModelItem>),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::Array<ruvia::String>));

RUVIA_RESPONSE_MODEL(NestedResponseItem, RUVIA_REQUIRED_FIELD(id, ruvia::UInt32),
    RUVIA_OPTIONAL_FIELD(label, ruvia::String));

RUVIA_RESPONSE_MODEL(NestedResponseEnvelope, RUVIA_REQUIRED_FIELD(primary, NestedResponseItem),
    RUVIA_REQUIRED_FIELD(items, ruvia::Array<NestedResponseItem>),
    RUVIA_OPTIONAL_FIELD(tags, ruvia::Array<ruvia::String>));

#define RUVIA_TEST_BOOL_FIELD(field) RUVIA_OPTIONAL_FIELD(field, ruvia::Bool)
RUVIA_RESPONSE_MODEL(UnlimitedFieldCountResponse, RUVIA_TEST_BOOL_FIELD(f01),
    RUVIA_TEST_BOOL_FIELD(f02), RUVIA_TEST_BOOL_FIELD(f03), RUVIA_TEST_BOOL_FIELD(f04),
    RUVIA_TEST_BOOL_FIELD(f05), RUVIA_TEST_BOOL_FIELD(f06), RUVIA_TEST_BOOL_FIELD(f07),
    RUVIA_TEST_BOOL_FIELD(f08), RUVIA_TEST_BOOL_FIELD(f09), RUVIA_TEST_BOOL_FIELD(f10),
    RUVIA_TEST_BOOL_FIELD(f11), RUVIA_TEST_BOOL_FIELD(f12), RUVIA_TEST_BOOL_FIELD(f13),
    RUVIA_TEST_BOOL_FIELD(f14), RUVIA_TEST_BOOL_FIELD(f15), RUVIA_TEST_BOOL_FIELD(f16),
    RUVIA_TEST_BOOL_FIELD(f17), RUVIA_TEST_BOOL_FIELD(f18), RUVIA_TEST_BOOL_FIELD(f19),
    RUVIA_TEST_BOOL_FIELD(f20), RUVIA_TEST_BOOL_FIELD(f21), RUVIA_TEST_BOOL_FIELD(f22),
    RUVIA_TEST_BOOL_FIELD(f23), RUVIA_TEST_BOOL_FIELD(f24), RUVIA_TEST_BOOL_FIELD(f25),
    RUVIA_TEST_BOOL_FIELD(f26), RUVIA_TEST_BOOL_FIELD(f27), RUVIA_TEST_BOOL_FIELD(f28),
    RUVIA_TEST_BOOL_FIELD(f29), RUVIA_TEST_BOOL_FIELD(f30), RUVIA_TEST_BOOL_FIELD(f31),
    RUVIA_TEST_BOOL_FIELD(f32), RUVIA_TEST_BOOL_FIELD(f33), RUVIA_TEST_BOOL_FIELD(f34),
    RUVIA_TEST_BOOL_FIELD(f35), RUVIA_TEST_BOOL_FIELD(f36), RUVIA_TEST_BOOL_FIELD(f37),
    RUVIA_TEST_BOOL_FIELD(f38), RUVIA_TEST_BOOL_FIELD(f39), RUVIA_TEST_BOOL_FIELD(f40),
    RUVIA_TEST_BOOL_FIELD(f41), RUVIA_TEST_BOOL_FIELD(f42), RUVIA_TEST_BOOL_FIELD(f43),
    RUVIA_TEST_BOOL_FIELD(f44), RUVIA_TEST_BOOL_FIELD(f45), RUVIA_TEST_BOOL_FIELD(f46),
    RUVIA_TEST_BOOL_FIELD(f47), RUVIA_TEST_BOOL_FIELD(f48), RUVIA_TEST_BOOL_FIELD(f49),
    RUVIA_TEST_BOOL_FIELD(f50), RUVIA_TEST_BOOL_FIELD(f51), RUVIA_TEST_BOOL_FIELD(f52),
    RUVIA_TEST_BOOL_FIELD(f53), RUVIA_TEST_BOOL_FIELD(f54), RUVIA_TEST_BOOL_FIELD(f55),
    RUVIA_TEST_BOOL_FIELD(f56), RUVIA_TEST_BOOL_FIELD(f57), RUVIA_TEST_BOOL_FIELD(f58),
    RUVIA_TEST_BOOL_FIELD(f59), RUVIA_TEST_BOOL_FIELD(f60), RUVIA_TEST_BOOL_FIELD(f61),
    RUVIA_TEST_BOOL_FIELD(f62), RUVIA_TEST_BOOL_FIELD(f63), RUVIA_TEST_BOOL_FIELD(f64),
    RUVIA_TEST_BOOL_FIELD(f65));
#undef RUVIA_TEST_BOOL_FIELD

static_assert(ruvia::detail::isResponseModel<UnlimitedFieldCountResponse>);

template <typename T>
concept ExposesAnyRvalueGeneratedMessageMember =
    requires { std::declval<const T&&>().template get<"message">(); } || requires {
        std::declval<T&&>().template ensure<"message">();
    } || requires { std::declval<T&&>().template set<"message">(std::string_view{}); };

static_assert(
    std::same_as<std::remove_cvref_t<
                     decltype(std::declval<AccessorSurfaceRequest&>().template get<"message">())>,
        std::optional<ruvia::String>>);
static_assert(
    std::same_as<std::remove_cvref_t<decltype(std::declval<const AccessorSurfaceRequest&>()
                         .template get<"message">())>,
        std::optional<ruvia::String>>);
static_assert(!ExposesAnyRvalueGeneratedMessageMember<AccessorSurfaceRequest>);
static_assert(!ExposesAnyRvalueGeneratedMessageMember<AccessorSurfaceResponse>);

}  // namespace model_field_test

using namespace model_field_test;  // NOLINT(google-build-using-namespace)
