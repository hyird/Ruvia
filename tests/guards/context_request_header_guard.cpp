#include "ruvia/web/ContextRequest.h"

#include <concepts>
#include <string_view>
#include <type_traits>

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

static_assert(sizeof(ruvia::ContextRequest) == sizeof(void*));
static_assert(std::same_as<
    decltype(std::declval<const ruvia::ContextRequest&>().method()),
    std::string_view>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::ContextRequest&>().parseBody()),
    ruvia::Task<ruvia::ContextRequest::RequestFormData>>);
static_assert(!HasRawRequestEscape<ruvia::ContextRequest>);
static_assert(!HasRequestCloneMethod<ruvia::ContextRequest>);
static_assert(!HasRawRequestCloneType<ruvia::ContextRequest>);

int main() {
    using Method = std::string_view (ruvia::ContextRequest::*)() const noexcept;
    volatile Method method = &ruvia::ContextRequest::method;
    return method == nullptr;
}
