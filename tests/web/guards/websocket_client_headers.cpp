#include <concepts>
#include <type_traits>

#include <ruvia/web/WebSocketClient.h>

static_assert(std::is_aggregate_v<ruvia::WebSocketClientConfig>);
static_assert(std::same_as<decltype(ruvia::WebSocketClientConfig{}.host), std::string>);
static_assert(std::same_as<decltype(ruvia::WebSocketClientConfig{}.headers),
    std::vector<std::pair<std::string, std::string>>>);
static_assert(std::same_as<decltype(ruvia::WebSocketClientConfig{}.tlsPeerVerification),
    ruvia::TlsPeerVerificationPolicy>);
static_assert(ruvia::WebSocketClientConfig{}.tlsPeerVerification ==
              ruvia::TlsPeerVerificationPolicy::kVerify);
static_assert(
    std::same_as<decltype(std::declval<ruvia::WebSocketClient&>().connect()), ruvia::Task<void>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::WebSocketClient&>().withOptions({})),
    ruvia::WebSocketClientHandle>);
static_assert(std::same_as<decltype(std::declval<const ruvia::WebSocketClient&>().read()),
    ruvia::ScopedOperation<std::optional<ruvia::WebSocketMessage>>>);
static_assert(
    std::same_as<decltype(std::declval<const ruvia::WebSocketClient&>().text(std::string_view{})),
        ruvia::ScopedOperation<void>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::WebSocketClient&>().close(
                               ruvia::WebSocketCloseOptions{})),
    ruvia::ScopedOperation<void>>);
static_assert(std::same_as<decltype(std::declval<ruvia::WebSocketClient&>().close()), void>);
static_assert(!std::copy_constructible<ruvia::WebSocketClient>);
static_assert(!std::move_constructible<ruvia::WebSocketClient>);

int main() {
    return 0;
}
