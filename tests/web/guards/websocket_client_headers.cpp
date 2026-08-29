#include <concepts>
#include <type_traits>

#include <ruvia/web/WebSocketClient.h>

template <typename Client>
concept HasRvalueWebSocketClientOperation = requires(Client&& client) { std::move(client).connect(); } || requires(Client&& client) { std::move(client).withOptions({}); } || requires(Client&& client) { std::move(client).read(); } || requires(Client&& client) { std::move(client).text(std::string_view{}); } || requires(Client&& client) { std::move(client).binary(std::string_view{}); } || requires(Client&& client) { std::move(client).ping(); } || requires(Client&& client) { std::move(client).pong(std::string_view{}); } || requires(Client&& client) { std::move(client).close(ruvia::WebSocketCloseOptions{}); };

template <typename Client>
concept HasNoArgumentClose = requires(Client& client) { client.close(); };

static_assert(std::is_aggregate_v<ruvia::WebSocketClientConfig>);
static_assert(std::same_as<decltype(ruvia::WebSocketClientConfig{}.host), std::string>);
static_assert(std::same_as<decltype(ruvia::WebSocketClientConfig{}.headers), std::vector<std::pair<std::string, std::string>>>);
static_assert(std::same_as<decltype(ruvia::WebSocketClientConfig{}.subprotocols), std::vector<std::string>>);
static_assert(std::same_as<decltype(ruvia::WebSocketClientConfig{}.heartbeat), ruvia::WebSocketHeartbeatConfig>);
static_assert(!ruvia::WebSocketClientConfig{}.heartbeat.pingInterval.has_value());
static_assert(std::same_as<decltype(ruvia::WebSocketClientConfig{}.tlsPeerVerification), ruvia::TlsPeerVerificationPolicy>);
static_assert(ruvia::WebSocketClientConfig{}.tlsPeerVerification == ruvia::TlsPeerVerificationPolicy::kVerify);
static_assert(std::same_as<decltype(std::declval<ruvia::WebSocketClient&>().connect()), ruvia::Task<void>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::WebSocketClient&>().withOptions({})), ruvia::WebSocketClientHandle>);
static_assert(std::same_as<decltype(std::declval<const ruvia::WebSocketClient&>().read()), ruvia::ScopedOperation<std::optional<ruvia::WebSocketMessage>>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::WebSocketClient&>().text(std::string_view{})), ruvia::ScopedOperation<void>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::WebSocketClient&>().close(ruvia::WebSocketCloseOptions{})), ruvia::ScopedOperation<void>>);
static_assert(std::same_as<decltype(std::declval<ruvia::WebSocketClient&>().abort()), void>);
static_assert(!HasNoArgumentClose<ruvia::WebSocketClient>);
static_assert(!HasNoArgumentClose<ruvia::WebSocketClientHandle>);
static_assert(!std::copy_constructible<ruvia::WebSocketClient>);
static_assert(!std::move_constructible<ruvia::WebSocketClient>);
static_assert(!HasRvalueWebSocketClientOperation<ruvia::WebSocketClient>);

int main() {
    return 0;
}
