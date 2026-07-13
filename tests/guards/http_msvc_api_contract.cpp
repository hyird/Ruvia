#include <concepts>
#include <cstddef>
#include <memory_resource>

#include <ruvia/http/detail/http1/Http1RequestBodyPlan.h>
#include <ruvia/http/detail/websocket/WebSocketServerNegotiation.h>
#include <ruvia/http/detail/websocket/WsConnection.h>

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
static_assert(!std::default_initializable<
    ruvia::detail::WebSocketServerNegotiation>);
static_assert(!std::constructible_from<
    ruvia::detail::WebSocketDeflateNegotiation,
    bool>);
static_assert(!std::constructible_from<
    ruvia::detail::WsConnection,
    std::pmr::string&,
    std::size_t,
    bool>);

int main() {}
