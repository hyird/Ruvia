#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <ruvia/http/Hpack.h>
#include <ruvia/http/HttpClient.h>
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

template <typename String>
concept AcceptsTemporaryHttpOriginHost = requires(String&& value) { ruvia::HttpOriginView::http({.host = std::forward<String>(value)}); };

template <typename String>
concept AcceptsLvalueHttpOriginHost = requires(String& value) { ruvia::HttpOriginView::http({.host = value}); };

template <typename String>
concept AcceptsHttpOriginPositionalFactories = requires(String value) { ruvia::HttpOriginView::http(value); } || requires(String value) { ruvia::HttpOriginView::http(value, std::uint16_t{80}); } || requires(String value) { ruvia::HttpOriginView::https(value); } || requires(String value) { ruvia::HttpOriginView::https(value, std::uint16_t{443}); };

static_assert(!HasPublicHttp1RequestBodyPlanFactories<ruvia::Http1RequestBodyPlan>);
static_assert(std::is_aggregate_v<ruvia::HttpOriginOptions>);
static_assert(std::same_as<decltype(ruvia::HttpOriginView::http({.host = "example.test"})), ruvia::HttpOriginView>);
static_assert(std::same_as<decltype(ruvia::HttpOriginView::https({.host = "example.test", .port = std::uint16_t{8443}})), ruvia::HttpOriginView>);
static_assert(!AcceptsHttpOriginPositionalFactories<std::string_view>);
static_assert(AcceptsLvalueHttpOriginHost<std::string>);
static_assert(!AcceptsTemporaryHttpOriginHost<std::string>);
static_assert(!AcceptsTemporaryHttpOriginHost<std::pmr::string>);
static_assert(!std::default_initializable<ruvia::Http1RequestWithoutBody>);
static_assert(!std::default_initializable<ruvia::Http1KnownLengthRequestBody>);
static_assert(!std::default_initializable<ruvia::Http1ChunkedRequestBody>);
static_assert(!std::default_initializable<ruvia::detail::WebSocketServerNegotiation>);
static_assert(!std::copy_constructible<ruvia::detail::WebSocketServerNegotiation>);
static_assert(std::move_constructible<ruvia::detail::WebSocketServerNegotiation>);
static_assert(!std::constructible_from<ruvia::WebSocketCompression, bool>);
static_assert(!std::constructible_from<ruvia::detail::WsConnection, std::pmr::string&, std::size_t, bool>);
static_assert(!std::copy_constructible<ruvia::Http2Connection>);
static_assert(!std::copy_constructible<ruvia::HpackDecoder>);
static_assert(!std::copy_constructible<ruvia::WebSocketServerConnection>);

int main() {}
