#include <concepts>
#include <cstddef>
#include <memory_resource>

#include <ruvia/http/Hpack.h>
#include <ruvia/http/Http2Connection.h>
#include <ruvia/http/Http2Framing.h>
#include <ruvia/http/Sse.h>
#include <ruvia/http/WebSocketServerConnection.h>
#include <ruvia/http/detail/http1/Http1RequestBodyPlan.h>
#include <ruvia/http/detail/websocket/handshake/WebSocketServerNegotiation.h>
#include <ruvia/http/detail/websocket/WsConnection.h>

template <typename T>
concept HasPublicHttp1RequestBodyPlanFactories = requires {
    T::makeWithoutBody();
    T::makeKnownLength(std::size_t{});
    T::makeChunked(ruvia::HttpTransferCodings{});
};

static_assert(!HasPublicHttp1RequestBodyPlanFactories<ruvia::Http1RequestBodyPlan>);
static_assert(!std::default_initializable<ruvia::Http1RequestWithoutBody>);
static_assert(!std::default_initializable<ruvia::Http1KnownLengthRequestBody>);
static_assert(!std::default_initializable<ruvia::Http1ChunkedRequestBody>);
static_assert(!std::default_initializable<ruvia::detail::WebSocketServerNegotiation>);
static_assert(!std::copy_constructible<ruvia::detail::WebSocketServerNegotiation>);
static_assert(std::move_constructible<ruvia::detail::WebSocketServerNegotiation>);
static_assert(!std::constructible_from<ruvia::detail::WebSocketDeflateNegotiation, bool>);
static_assert(!std::constructible_from<ruvia::detail::WsConnection, std::pmr::string&, std::size_t, bool>);
static_assert(!std::copy_constructible<ruvia::Http2Connection>);
static_assert(!std::copy_constructible<ruvia::HpackDecoder>);
static_assert(!std::copy_constructible<ruvia::WebSocketServerConnection>);

int main() {}
