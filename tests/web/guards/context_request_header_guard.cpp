#include "ruvia/web/ContextRequest.h"
#include "ruvia/web/detail/http/request/RequestFieldsAccess.h"

#include <concepts>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

template <typename Request>
concept HasRawRequestEscape = requires(const Request& request) {
    request.raw();
};

template <typename Request>
concept HasRequestCloneMethod = requires(const Request& request) {
    request.clone();
};

template <typename Request>
concept HasRawRequestCloneType = requires {
    typename Request::RawRequestClone;
};

template <typename Field>
concept ExposesAnyRvalueRequestFormFieldBorrow =
    requires { std::declval<const Field&&>().name(); } ||
    requires { std::declval<const Field&&>().value(); } ||
    requires { std::declval<const Field&&>().filename(); } ||
    requires { std::declval<const Field&&>().contentType(); } ||
    requires { std::declval<const Field&&>().path(); } ||
    requires { std::declval<const Field&&>().blob(); };

template <typename Entry>
concept ExposesRvalueRequestFormEntryFields = requires {
    std::declval<const Entry&&>().fields();
};

template <typename Form>
concept ExposesAnyRvalueRequestFormDataBorrow =
    requires { std::declval<const Form&&>().fields(); } ||
    requires { std::declval<const Form&&>().groups(); } ||
    requires { std::declval<const Form&&>().get(std::string_view{}); } ||
    requires { std::declval<const Form&&>().object(std::string_view{}); };

template <typename Object>
concept ExposesRvalueRequestFormObjectGroups = requires {
    std::declval<const Object&&>().groups();
};

template <typename List>
concept ExposesAnyRvalueRequestNameValueListBorrow =
    requires { std::declval<const List&&>().begin(); } ||
    requires { std::declval<const List&&>().cbegin(); } ||
    requires { std::declval<const List&&>().end(); } ||
    requires { std::declval<const List&&>().cend(); } ||
    requires { std::declval<const List&&>().data(); } ||
    requires { std::declval<const List&&>()[std::size_t{}]; } ||
    requires { std::declval<const List&&>().entries(); };

template <typename Input>
concept AcceptsRequestFieldName = requires(Input&& input) {
    ruvia::detail::RequestNameValueViewAccess::make(
        std::forward<Input>(input), std::string_view{});
};

template <typename Input>
concept AcceptsRequestFieldValue = requires(Input&& input) {
    ruvia::detail::RequestNameValueViewAccess::make(
        std::string_view{}, std::forward<Input>(input));
};

static_assert(sizeof(ruvia::ContextRequest) == sizeof(void*));
static_assert(std::same_as<
    decltype(std::declval<const ruvia::ContextRequest&>().method()),
    std::string_view>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::ContextRequest&>().parseBody()),
    ruvia::ScopedOperation<ruvia::ContextRequest::RequestFormData>>);
static_assert(!HasRawRequestEscape<ruvia::ContextRequest>);
static_assert(!HasRequestCloneMethod<ruvia::ContextRequest>);
static_assert(!HasRawRequestCloneType<ruvia::ContextRequest>);
static_assert(!ExposesAnyRvalueRequestFormFieldBorrow<
    ruvia::ContextRequest::RequestFormField>);
static_assert(!ExposesRvalueRequestFormEntryFields<
    ruvia::ContextRequest::RequestFormData::Entry>);
static_assert(!ExposesAnyRvalueRequestFormDataBorrow<
    ruvia::ContextRequest::RequestFormData>);
static_assert(!ExposesRvalueRequestFormObjectGroups<
    ruvia::ContextRequest::RequestFormData::Object>);
static_assert(!ExposesAnyRvalueRequestNameValueListBorrow<
    ruvia::RequestNameValueList>);
static_assert(!AcceptsRequestFieldName<std::string>);
static_assert(!AcceptsRequestFieldName<const std::string>);
static_assert(!AcceptsRequestFieldName<std::pmr::string>);
static_assert(AcceptsRequestFieldName<std::string&>);
static_assert(AcceptsRequestFieldName<std::pmr::string&>);
static_assert(AcceptsRequestFieldName<std::string_view>);
static_assert(!AcceptsRequestFieldValue<std::string>);
static_assert(!AcceptsRequestFieldValue<const std::string>);
static_assert(!AcceptsRequestFieldValue<std::pmr::string>);
static_assert(AcceptsRequestFieldValue<std::string&>);
static_assert(AcceptsRequestFieldValue<std::pmr::string&>);
static_assert(AcceptsRequestFieldValue<std::string_view>);

int main() {
    using Method = std::string_view (ruvia::ContextRequest::*)() const noexcept;
    volatile Method method = &ruvia::ContextRequest::method;
    return method == nullptr;
}
