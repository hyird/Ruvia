// RUVIA_MODEL schema, ownership, and generated API surface.
#include <concepts>
#include <cstddef>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/Model.h"

namespace {

struct ClonePayload final {
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_MODEL(ClonePayload, message);
};

struct SurfaceJsonResponse final {
    RUVIA_OPTIONAL_FIELD(message, ruvia::String);
    RUVIA_MODEL(SurfaceJsonResponse, message);
};

template <typename T>
concept HasModelInputAccessor = requires(const T& model) { model.body(); };

template <typename T>
concept HasModelDynamicGet = requires(const T& model) { model.get(std::string_view{}); };

template <typename T>
concept HasModelTypedDynamicGet = requires(const T& model) { model.template get<ruvia::String>(std::string_view{}); };

template <typename T>
concept HasModelCompileTimeGetAlias = requires(const T& model) { model.template get<"message">(); };

template <typename T>
concept HasModelPublicBodyParseHooks = requires { T::ruviaParseJsonBody(std::string_view{}, static_cast<std::pmr::memory_resource*>(nullptr)); } || requires { T::ruviaParseFormBody(std::string_view{}, static_cast<std::pmr::memory_resource*>(nullptr)); };

template <typename T>
concept HasModelPublicJsonDepthHook = requires { T::ruviaParseJsonBodyDepth(std::string_view{}, static_cast<std::pmr::memory_resource*>(nullptr), std::size_t{}); };

template <typename T>
concept HasModelPublicFormFieldsHook = requires { T::ruviaParseFormFields(std::declval<const ruvia::RequestNameValueList&>(), static_cast<std::pmr::memory_resource*>(nullptr)); };

template <typename T>
concept HasModelNonConstMessageGetter = requires { static_cast<const std::optional<ruvia::String> & (T::*)()>(&T::message); };

template <typename T>
concept HasModelPublicJsonWriterHooks = requires(const T& model, std::pmr::string& output) { model.ruviaAppendJson(output); } || requires(const T& model) { model.ruviaJsonSizeHint(); };

template <typename T>
concept HasModelPublicFieldStateHook = requires(const T& model) { model.template ruviaFieldState<"message">(); };

template <typename T>
concept ExposesAnyRvalueModelStringBorrow = requires { std::declval<const T&&>().view(); } || requires { std::declval<const T&&>().data(); } || requires { static_cast<std::string_view>(std::declval<const T&&>()); };

template <typename T>
concept ExposesRvalueFixedStringView = requires { std::declval<const T&&>().view(); };

template <typename T>
concept ExposesAnyRvalueModelListBorrow = requires { std::declval<const T&&>()[std::size_t{}]; } || requires { std::declval<const T&&>().front(); } || requires { std::declval<const T&&>().begin(); } || requires { std::declval<const T&&>().end(); } || requires { std::declval<T&&>().emplace(1); } || requires { std::declval<T&&>().emplaceMove(typename T::value_type{}); };

template <typename T>
concept ExposesAnyRvalueGeneratedMessageMember = requires { std::declval<const T&&>().message(); } || requires { std::declval<T&&>().messageEnsure(); } || requires { std::declval<T&&>().message(std::string_view{}); };

struct ModelBodyDuckProbe final {
    static int ruviaParseJsonBody(std::string_view, std::pmr::memory_resource*);
    static int ruviaParseFormBody(std::string_view, std::pmr::memory_resource*);
};

static_assert(!std::is_default_constructible_v<ruvia::JsonValue>);
static_assert(!std::is_constructible_v<ruvia::JsonValue, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::JsonValue, std::string_view, std::pmr::memory_resource*>);
static_assert(!std::is_default_constructible_v<ruvia::JsonObject>);
static_assert(!std::is_constructible_v<ruvia::JsonObject, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::JsonObject, std::string_view, std::pmr::memory_resource*>);
static_assert(!std::is_default_constructible_v<ruvia::FormObject>);
static_assert(!std::is_constructible_v<ruvia::FormObject, std::string_view>);
static_assert(!std::is_constructible_v<ruvia::FormObject, std::string_view, std::pmr::memory_resource*>);
static_assert(!std::is_default_constructible_v<ruvia::detail::ModelInput>);
static_assert(!std::is_constructible_v<ruvia::detail::ModelInput, ruvia::detail::ModelInputKind, std::string_view, std::pmr::memory_resource*>);
static_assert(!std::is_constructible_v<ruvia::detail::ModelInput, const ruvia::RequestNameValueList&, std::pmr::memory_resource*>);
static_assert(!HasModelInputAccessor<ClonePayload>);
static_assert(!HasModelDynamicGet<ClonePayload>);
static_assert(!HasModelTypedDynamicGet<ClonePayload>);
static_assert(!HasModelCompileTimeGetAlias<ClonePayload>);
static_assert(!HasModelPublicBodyParseHooks<ClonePayload>);
static_assert(!HasModelPublicJsonDepthHook<ClonePayload>);
static_assert(!HasModelPublicFormFieldsHook<ClonePayload>);
static_assert(!HasModelNonConstMessageGetter<ClonePayload>);
static_assert(!HasModelPublicJsonWriterHooks<ClonePayload>);
static_assert(!HasModelPublicFieldStateHook<ClonePayload>);
static_assert(!ExposesAnyRvalueModelStringBorrow<ruvia::String>);
static_assert(!ExposesRvalueFixedStringView<ruvia::FixedString<6>>);
static_assert(!ExposesAnyRvalueModelListBorrow<ruvia::List<ruvia::Int32>>);
static_assert(!ExposesAnyRvalueGeneratedMessageMember<ClonePayload>);
static_assert(!ExposesAnyRvalueGeneratedMessageMember<SurfaceJsonResponse>);
static_assert(ruvia::JsonBody<ClonePayload>::value);
static_assert(ruvia::detail::isResponseModel<ClonePayload>);
static_assert(ruvia::JsonBody<SurfaceJsonResponse>::value);
static_assert(ruvia::FormBody<SurfaceJsonResponse>::value);
static_assert(ruvia::detail::isResponseModel<SurfaceJsonResponse>);
static_assert(!ruvia::JsonBody<ModelBodyDuckProbe>::value);
static_assert(!ruvia::FormBody<ModelBodyDuckProbe>::value);
static_assert(!std::is_constructible_v<ClonePayload, ruvia::detail::ModelInput>);

}  // namespace

int main() {
    return 0;
}
