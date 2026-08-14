// Request/response model roles, compile-time field access, and legacy API guard.
#include <concepts>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/Model.h"

namespace {

RUVIA_REQUEST_MODEL(ClonePayload,
    RUVIA_OPTIONAL_FIELD(message, ruvia::String));

RUVIA_RESPONSE_MODEL(SurfaceJsonResponse,
    RUVIA_OPTIONAL_FIELD(message, ruvia::String),
    RUVIA_OPTIONAL_FIELD(nullable, ruvia::String, RUVIA_EMIT_NULL));

RUVIA_RESPONSE_MODEL(RequiredFieldSurface,
    RUVIA_REQUIRED_FIELD(id, ruvia::UInt32),
    RUVIA_OPTIONAL_FIELD(label, ruvia::String));

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
concept CanFromForm = requires { ruvia::fromForm<T>(""); };

template <typename T>
concept CanToJson = requires(const T& value) { ruvia::toJson(value); };

template <typename T>
concept HasPublicParseHook = requires {
    T::ruviaParseJsonBody(
        std::string_view{},
        static_cast<std::pmr::memory_resource*>(nullptr));
};

template <typename T>
concept HasPublicWriterHook = requires(const T& model, std::pmr::string& output) {
    model.ruviaAppendJson(output);
};

template <typename T>
concept HasPublicFieldStateHook = requires(const T& model) {
    model.template ruviaFieldState<"message">();
};

static_assert(ruvia::JsonBody<ClonePayload>::value);
static_assert(ruvia::FormBody<ClonePayload>::value);
static_assert(ruvia::detail::isRequestModel<ClonePayload>);
static_assert(!ruvia::detail::isResponseModel<ClonePayload>);
static_assert(CanFromJson<ClonePayload>);
static_assert(CanFromForm<ClonePayload>);
static_assert(!CanToJson<ClonePayload>);

static_assert(!ruvia::JsonBody<SurfaceJsonResponse>::value);
static_assert(!ruvia::FormBody<SurfaceJsonResponse>::value);
static_assert(!ruvia::detail::isRequestModel<SurfaceJsonResponse>);
static_assert(ruvia::detail::isResponseModel<SurfaceJsonResponse>);
static_assert(!CanFromJson<SurfaceJsonResponse>);
static_assert(!CanFromForm<SurfaceJsonResponse>);
static_assert(CanToJson<SurfaceJsonResponse>);
static_assert(!CanToJson<std::uint32_t>);
static_assert(!CanToJson<ruvia::String>);
static_assert(!CanToJson<ruvia::Array<ruvia::String>>);

static_assert(!HasLegacyMessageAccessor<ClonePayload>);
static_assert(!HasLegacyMessageAccessor<SurfaceJsonResponse>);
static_assert(!HasDynamicGet<ClonePayload>);
static_assert(!HasRvalueGet<ClonePayload>);
static_assert(!HasRvalueSet<SurfaceJsonResponse>);
static_assert(!HasRvalueEnsure<SurfaceJsonResponse>);
static_assert(!HasPublicParseHook<ClonePayload>);
static_assert(!HasPublicWriterHook<SurfaceJsonResponse>);
static_assert(!HasPublicFieldStateHook<ClonePayload>);

static_assert(std::same_as<
              decltype(std::declval<const RequiredFieldSurface&>().template get<"id">()),
              const ruvia::UInt32&>);
static_assert(std::same_as<
              decltype(std::declval<const RequiredFieldSurface&>().template get<"label">()),
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
