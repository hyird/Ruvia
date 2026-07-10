#include "test_harness.h"

#include <concepts>
#include <initializer_list>
#include <span>
#include <string_view>

#include "ruvia/web/db/Db.h"

namespace {

template <typename T>
concept HasDbHandleDefaultParams = requires(const T& handle) {
    handle.query(std::string_view{});
    handle.execute(std::string_view{});
    handle.queryStream(std::string_view{});
};

template <typename T>
concept HasDbHandleSpanParams = requires(
    const T& handle,
    std::span<const ruvia::DbValue> params) {
    handle.query(std::string_view{}, params);
    handle.execute(std::string_view{}, params);
    handle.queryStream(std::string_view{}, params);
};

template <typename T>
concept HasDbHandleInitializerListParams = requires(
    const T& handle,
    std::initializer_list<ruvia::DbValue> params) {
    handle.query(std::string_view{}, params);
    handle.execute(std::string_view{}, params);
    handle.queryStream(std::string_view{}, params);
};

template <typename T>
concept HasDbTransactionDefaultParams = requires(T& transaction) {
    transaction.query(std::string_view{});
    transaction.execute(std::string_view{});
};

template <typename T>
concept HasDbTransactionSpanParams = requires(
    T& transaction,
    std::span<const ruvia::DbValue> params) {
    transaction.query(std::string_view{}, params);
    transaction.execute(std::string_view{}, params);
};

template <typename T>
concept HasDbTransactionInitializerListParams = requires(
    T& transaction,
    std::initializer_list<ruvia::DbValue> params) {
    transaction.query(std::string_view{}, params);
    transaction.execute(std::string_view{}, params);
};

static_assert(HasDbHandleDefaultParams<ruvia::DbHandle>);
static_assert(HasDbHandleSpanParams<ruvia::DbHandle>);
static_assert(!HasDbHandleInitializerListParams<ruvia::DbHandle>);
static_assert(HasDbTransactionDefaultParams<ruvia::DbTransaction>);
static_assert(HasDbTransactionSpanParams<ruvia::DbTransaction>);
static_assert(!HasDbTransactionInitializerListParams<ruvia::DbTransaction>);

}  // namespace

RUVIA_TEST(db_api_surface_uses_span_params_without_initializer_list_overloads) {
    RUVIA_CHECK(true);
}
