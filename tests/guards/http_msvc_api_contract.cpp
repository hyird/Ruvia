#include <concepts>
#include <cstddef>

#include <ruvia/http/detail/http1/Http1RequestBodyPlan.h>

template <typename T>
concept HasPublicHttp1RequestBodyPlanFactories = requires {
    T::makeWithoutBody();
    T::makeKnownLength(std::size_t{});
    T::makeChunked(ruvia::detail::HttpTransferCodings{});
};

static_assert(!HasPublicHttp1RequestBodyPlanFactories<
    ruvia::detail::Http1RequestBodyPlan>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1RequestWithoutBody>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1KnownLengthRequestBody>);
static_assert(!std::default_initializable<
    ruvia::detail::Http1ChunkedRequestBody>);

int main() {}
