#include "ruvia/web/ContextRequest.h"

#include <concepts>
#include <string_view>
#include <type_traits>

static_assert(sizeof(ruvia::ContextRequest) == sizeof(void*));
static_assert(std::same_as<
    decltype(std::declval<const ruvia::ContextRequest&>().method()),
    std::string_view>);
static_assert(std::same_as<
    decltype(std::declval<const ruvia::ContextRequest&>().parseBody()),
    ruvia::Task<ruvia::ContextRequest::RequestFormData>>);
static_assert(!std::is_copy_constructible_v<
    ruvia::ContextRequest::RawRequestClone>);

int main() {}
