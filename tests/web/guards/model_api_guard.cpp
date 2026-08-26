// Request/response model roles, compile-time field access, and legacy API guard.
#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/Context.h"
#include "ruvia/web/Model.h"

namespace {

RUVIA_REQUEST_MODEL(ClonePayload, RUVIA_OPTIONAL_FIELD(message, ruvia::String));

RUVIA_RESPONSE_MODEL(SurfaceJsonResponse, RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(nullable, ruvia::String, RUVIA_EMIT_NULL));

RUVIA_RESPONSE_MODEL(RequiredFieldSurface, RUVIA_REQUIRED_FIELD(id, ruvia::UInt32),
    RUVIA_OPTIONAL_FIELD(label, ruvia::String));

struct DirectRequestModel final
    : ruvia::RequestModel<DirectRequestModel, RUVIA_OPTIONAL_FIELD(message, ruvia::String)> {
    using RuviaModelBase::RuviaModelBase;
};

struct DirectResponseModel final
    : ruvia::ResponseModel<DirectResponseModel, RUVIA_OPTIONAL_FIELD(message, ruvia::String)> {
    using RuviaModelBase::RuviaModelBase;
};

template <typename T>
concept HasLegacyMessageAccessor = requires(const T& model) { model.message(); };

template <typename T>
concept HasDynamicGet = requires(const T& model) { model.get(std::string_view{}); };

template <typename T>
concept HasRvalueGet = requires { std::declval<const T&&>().template get<"message">(); };

template <typename T>
concept HasRvalueSet = requires { std::declval<T&&>().template set<"message">("value"); };

template <typename T>
concept HasRvalueEnsure = requires { std::declval<T&&>().template ensure<"message">(); };

template <typename T>
concept HasRequiredReset = requires(T& model) { model.template reset<"id">(); };

template <typename T>
concept HasOptionalReset = requires(T& model) { model.template reset<"label">(); };

template <typename T>
concept CanFromJson = requires { ruvia::fromJson<T>("{}"); };

template <typename T>
concept CanFromJsonWithOptions = requires(std::pmr::memory_resource* resource) {
    ruvia::fromJson<T>("{}", ruvia::ModelParseOptions{.resource = resource});
};

template <typename T>
concept CanFromJsonWithPositionalResource =
    requires(std::pmr::memory_resource* resource) { ruvia::fromJson<T>("{}", resource); };

template <typename T>
concept CanFromForm = requires { ruvia::fromForm<T>(""); };

template <typename T>
concept CanFromFormWithOptions = requires(std::pmr::memory_resource* resource) {
    ruvia::fromForm<T>("", ruvia::ModelParseOptions{.resource = resource});
};

template <typename T>
concept CanFromFormWithPositionalResource =
    requires(std::pmr::memory_resource* resource) { ruvia::fromForm<T>("", resource); };

template <typename T>
concept CanToJson = requires(const T& value) { ruvia::toJson(value); };

template <typename T>
concept CanToJsonWithOptions = requires(const T& value, std::pmr::memory_resource* resource) {
    ruvia::toJson(value, ruvia::ModelSerializeOptions{.resource = resource});
};

template <typename T>
concept CanToJsonWithPositionalResource = requires(
    const T& value, std::pmr::memory_resource* resource) { ruvia::toJson(value, resource); };

template <typename T>
concept HasPublicParseHook = requires {
    T::ruviaParseJsonBody(std::string_view{}, static_cast<std::pmr::memory_resource*>(nullptr));
};

template <typename T>
concept HasPublicWriterHook =
    requires(const T& model, std::pmr::string& output) { model.ruviaAppendJson(output); };

template <typename T>
concept HasPublicFieldStateHook =
    requires(const T& model) { model.template ruviaFieldState<"message">(); };

template <typename String>
concept AcceptsTemporaryRuleMessage =
    requires(String&& value) { ruvia::detail::model::Required{std::forward<String>(value)}; };

template <typename T>
concept HasModelOptionsConstructor =
    requires(std::pmr::memory_resource* resource) { T(ruvia::ModelOptions{.resource = resource}); };

template <typename T>
concept HasPositionalModelResourceConstructor =
    requires(std::pmr::memory_resource* resource) { T(resource); };

template <typename T>
concept HasPositionalStringValueResourceConstructor =
    requires(std::pmr::memory_resource* resource) { T(std::string_view{}, resource); };

template <typename T>
concept HasModelStringOptionsConstructor = requires(std::pmr::memory_resource* resource) {
    T(std::string_view{}, ruvia::ModelOptions{.resource = resource});
};

template <typename Parser>
concept HasPositionalModelObjectParseResource =
    requires(std::pmr::memory_resource* resource) { Parser::parse(std::string_view{}, resource); };

template <typename Parser>
concept HasModelObjectParseOptions = requires(std::pmr::memory_resource* resource) {
    Parser::parse(std::string_view{}, ruvia::ModelParseOptions{.resource = resource});
};

static_assert(std::is_aggregate_v<ruvia::ModelOptions>);
static_assert(std::same_as<decltype(ruvia::ModelOptions{}.resource), std::pmr::memory_resource*>);
static_assert(std::is_aggregate_v<ruvia::ModelParseOptions>);
static_assert(
    std::same_as<decltype(ruvia::ModelParseOptions{}.resource), std::pmr::memory_resource*>);
static_assert(std::is_aggregate_v<ruvia::ModelSerializeOptions>);
static_assert(
    std::same_as<decltype(ruvia::ModelSerializeOptions{}.resource), std::pmr::memory_resource*>);
static_assert(
    std::is_same_v<decltype(ruvia::detail::model::Required{}.message), ruvia::BorrowedText>);
static_assert(std::is_same_v<decltype(ruvia::detail::model::Min{}.message), ruvia::BorrowedText>);
static_assert(std::is_same_v<decltype(ruvia::detail::model::Max{}.message), ruvia::BorrowedText>);
static_assert(std::is_same_v<decltype(ruvia::detail::model::Email{}.message), ruvia::BorrowedText>);
static_assert(
    std::is_same_v<decltype(ruvia::detail::model::RegexRule<"x">{}.message), ruvia::BorrowedText>);
static_assert(!AcceptsTemporaryRuleMessage<std::string>);
static_assert(!AcceptsTemporaryRuleMessage<const std::string>);

static_assert(ruvia::JsonBody<ClonePayload>::value);
static_assert(ruvia::FormBody<ClonePayload>::value);
static_assert(ruvia::detail::isRequestModel<ClonePayload>);
static_assert(!ruvia::detail::isResponseModel<ClonePayload>);
static_assert(CanFromJson<ClonePayload>);
static_assert(CanFromJsonWithOptions<ClonePayload>);
static_assert(!CanFromJsonWithPositionalResource<ClonePayload>);
static_assert(CanFromForm<ClonePayload>);
static_assert(CanFromFormWithOptions<ClonePayload>);
static_assert(!CanFromFormWithPositionalResource<ClonePayload>);
static_assert(!CanToJson<ClonePayload>);

static_assert(!ruvia::JsonBody<SurfaceJsonResponse>::value);
static_assert(!ruvia::FormBody<SurfaceJsonResponse>::value);
static_assert(!ruvia::detail::isRequestModel<SurfaceJsonResponse>);
static_assert(ruvia::detail::isResponseModel<SurfaceJsonResponse>);
static_assert(!CanFromJson<SurfaceJsonResponse>);
static_assert(!CanFromForm<SurfaceJsonResponse>);
static_assert(CanToJson<SurfaceJsonResponse>);
static_assert(CanToJsonWithOptions<SurfaceJsonResponse>);
static_assert(!CanToJsonWithPositionalResource<SurfaceJsonResponse>);
static_assert(!CanToJson<std::uint32_t>);
static_assert(!CanToJson<ruvia::String>);
static_assert(!CanToJson<ruvia::Array<ruvia::String>>);

static_assert(ruvia::detail::isRequestModel<DirectRequestModel>);
static_assert(!ruvia::detail::isResponseModel<DirectRequestModel>);
static_assert(!ruvia::detail::isRequestModel<DirectResponseModel>);
static_assert(ruvia::detail::isResponseModel<DirectResponseModel>);
static_assert(HasModelOptionsConstructor<ClonePayload>);
static_assert(HasModelOptionsConstructor<SurfaceJsonResponse>);
static_assert(HasModelOptionsConstructor<ruvia::String>);
static_assert(HasModelStringOptionsConstructor<ruvia::String>);
static_assert(HasModelOptionsConstructor<ruvia::BoxedArray<ruvia::String>>);
static_assert(!HasPositionalModelResourceConstructor<ClonePayload>);
static_assert(!HasPositionalModelResourceConstructor<SurfaceJsonResponse>);
static_assert(!HasPositionalModelResourceConstructor<ruvia::String>);
static_assert(!HasPositionalStringValueResourceConstructor<ruvia::String>);
static_assert(!HasPositionalModelResourceConstructor<ruvia::BoxedArray<ruvia::String>>);
static_assert(HasModelObjectParseOptions<ruvia::JsonValue>);
static_assert(HasModelObjectParseOptions<ruvia::JsonObject>);
static_assert(HasModelObjectParseOptions<ruvia::FormObject>);
static_assert(!HasPositionalModelObjectParseResource<ruvia::JsonValue>);
static_assert(!HasPositionalModelObjectParseResource<ruvia::JsonObject>);
static_assert(!HasPositionalModelObjectParseResource<ruvia::FormObject>);

static_assert(!HasLegacyMessageAccessor<ClonePayload>);
static_assert(!HasLegacyMessageAccessor<SurfaceJsonResponse>);
static_assert(!HasDynamicGet<ClonePayload>);
static_assert(!HasRvalueGet<ClonePayload>);
static_assert(!HasRvalueSet<SurfaceJsonResponse>);
static_assert(!HasRvalueEnsure<SurfaceJsonResponse>);
static_assert(!HasPublicParseHook<ClonePayload>);
static_assert(!HasPublicWriterHook<SurfaceJsonResponse>);
static_assert(!HasPublicFieldStateHook<ClonePayload>);

[[maybe_unused]] ruvia::Task<ruvia::HttpResponse> responseModelSmokeTest(ruvia::Context& context) {
    SurfaceJsonResponse response;
    response.set<"message">("ok");
    co_return context.json(response);
}

static_assert(
    std::same_as<decltype(std::declval<const RequiredFieldSurface&>().template get<"id">()),
        const ruvia::UInt32&>);
static_assert(
    std::same_as<decltype(std::declval<const RequiredFieldSurface&>().template get<"label">()),
        const std::optional<ruvia::String>&>);
static_assert(!HasRequiredReset<RequiredFieldSurface>);
static_assert(HasOptionalReset<RequiredFieldSurface>);

}  // namespace

int main() {
    SurfaceJsonResponse response;
    response.set<"message">("ok");
    const auto json = ruvia::toJson(response);
    return std::string_view(json) == R"({"message":"ok","nullable":null})" ? 0 : 1;
}
