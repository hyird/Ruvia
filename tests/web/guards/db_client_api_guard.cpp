#include <concepts>
#include <initializer_list>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/App.h"
#include "ruvia/web/db/DbClient.h"

namespace {

template <typename T>
concept HasDefaultOperations = requires(const T& client) {
    { client.query(std::string_view{}) } -> std::same_as<ruvia::ScopedOperation<ruvia::DbRows>>;
    { client.execute(std::string_view{}) } -> std::same_as<ruvia::ScopedOperation<ruvia::DbExecResult>>;
    { client.queryStream(std::string_view{}) } -> std::same_as<ruvia::ScopedOperation<ruvia::DbStreamResult>>;
    { client.beginTransaction() } -> std::same_as<ruvia::ScopedOperation<ruvia::DbTransaction>>;
};

template <typename T>
concept HasSpanOperations = requires(const T& client, std::span<const ruvia::DbValue> params) {
    client.query(std::string_view{}, params);
    client.execute(std::string_view{}, params);
    client.queryStream(std::string_view{}, params);
};

template <typename T>
concept HasInitializerListOperations = requires(const T& client, std::initializer_list<ruvia::DbValue> params) {
    client.query(std::string_view{}, params);
    client.execute(std::string_view{}, params);
    client.queryStream(std::string_view{}, params);
};

template <typename T>
concept HasVariadicOperations = requires(const T& client) {
    client.query(std::string_view{}, 1, std::string_view{});
    client.execute(std::string_view{}, 1, std::string_view{});
    client.queryStream(std::string_view{}, 1, std::string_view{});
};

template <typename T>
concept HasDbRegistrationConfig = requires(T& app, ruvia::DbConfig config) {
    { app.database(ruvia::DbRegistrationConfig{.config = config}) } -> std::same_as<ruvia::App&>;
};

template <typename T>
concept HasDbRegistrationPositional = requires(T& app, ruvia::DbConfig config) { app.database(config); } || requires(T& app, ruvia::DbConfig config) { app.database(std::string_view{}, config); };

static_assert(std::is_aggregate_v<ruvia::DbRegistrationConfig>);
static_assert(std::same_as<decltype(ruvia::DbRegistrationConfig{.config = std::declval<ruvia::DbConfig>()}.alias), std::string>);
static_assert(HasDbRegistrationConfig<ruvia::App>);
static_assert(!HasDbRegistrationPositional<ruvia::App>);

static_assert(std::constructible_from<ruvia::DbClient, ruvia::EventLoop, ruvia::DbConfig>);
static_assert(!std::default_initializable<ruvia::DbClient>);
static_assert(!std::copy_constructible<ruvia::DbClient>);
static_assert(!std::move_constructible<ruvia::DbClient>);
static_assert(std::same_as<decltype(std::declval<ruvia::DbClient&>().connect()), ruvia::Task<void>>);
static_assert(std::same_as<decltype(std::declval<const ruvia::DbClient&>().withOptions(ruvia::OperationOptions{})), ruvia::DbHandle>);
static_assert(std::same_as<decltype(std::declval<const ruvia::DbClient&>().worker()), const ruvia::WorkerHandle&>);
static_assert(HasDefaultOperations<ruvia::DbClient>);
static_assert(HasSpanOperations<ruvia::DbClient>);
static_assert(!HasInitializerListOperations<ruvia::DbClient>);
static_assert(HasVariadicOperations<ruvia::DbClient>);

}  // namespace

int main() {
#ifdef RUVIA_ENABLE_MARIADB
    auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb};
#else
    auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql};
#endif
    try {
        ruvia::DbClient invalid(ruvia::EventLoop{}, std::move(config));
        return 1;
    } catch (const std::invalid_argument&) {
        return 0;
    }
}
