#include "test_harness.h"

#include <array>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/db/Db.h"
#include "ruvia/web/detail/db/DbResultAccess.h"

namespace {

class RejectingMemoryResource final : public std::pmr::memory_resource {
public:
    void rejectAllocations(bool value = true) noexcept {
        rejecting_ = value;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (rejecting_) {
            throw std::bad_alloc();
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* value, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(value, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool rejecting_{false};
};

static_assert(std::is_move_assignable_v<ruvia::DbField>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::DbField>);
static_assert(std::is_move_assignable_v<ruvia::DbRow>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::DbRow>);
static_assert(std::is_move_constructible_v<ruvia::DbMigrationReport>);
static_assert(!std::is_move_assignable_v<ruvia::DbMigrationReport>);
static_assert(std::is_move_constructible_v<ruvia::QueryResult>);
static_assert(!std::is_move_assignable_v<ruvia::QueryResult>);
static_assert(std::is_move_constructible_v<ruvia::DbStreamResult>);
static_assert(!std::is_move_assignable_v<ruvia::DbStreamResult>);
static_assert(std::is_move_constructible_v<ruvia::DbTransaction>);
static_assert(!std::is_move_assignable_v<ruvia::DbTransaction>);

template <typename T>
concept ExposesAnyRvalueDbOwnedView =
    requires(T&& value) { std::move(value).text(); } ||
    requires(T&& value) { std::move(value)[std::size_t{}]; } ||
    requires(T&& value) { std::move(value).begin(); } ||
    requires(T&& value) { std::move(value).end(); } ||
    requires(T&& value) { std::move(value).rows(); } ||
    requires(T&& value) { std::move(value).applied(); } ||
    requires(T&& value) { std::move(value).skipped(); };

static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::DbValue>);
static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::DbField>);
static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::DbRow>);
static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::QueryResult>);
static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::DbMigrationReport>);

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

RUVIA_TEST(db_query_result_move_transfers_direct_raii_ownership) {
    int releases = 0;
    {
        auto result = ruvia::detail::DbResultAccess::makeResult(nullptr);
        ruvia::detail::DbResultAccess::setAffectedRows(result, 7);
        ruvia::detail::DbResultAccess::retainRawResult(
            result,
            &releases,
            [](void* value) noexcept {
                ++*static_cast<int*>(value);
            });

        auto moved = std::move(result);
        RUVIA_CHECK_EQ(moved.affectedRows(), std::uint64_t{7});
        RUVIA_CHECK_EQ(releases, 0);
    }
    RUVIA_CHECK_EQ(releases, 1);
}

RUVIA_TEST(db_migrator_validates_before_opening_connection) {
    const std::array<ruvia::DbMigration, 2> migrations{{
        ruvia::DbMigration{"duplicate", "SELECT 1"},
        ruvia::DbMigration{"duplicate", "SELECT 2"},
    }};
    bool rejected = false;
    try {
        (void)ruvia::DbMigrator::migrate(
            ruvia::DbConfig{},
            std::span<const ruvia::DbMigration>(migrations));
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) ==
            "database migration ids must be unique";
    }
    RUVIA_CHECK(rejected);
}

RUVIA_TEST(db_result_value_move_assignment_propagates_allocator_failure) {
    RejectingMemoryResource rejecting;
    auto destination = ruvia::detail::DbResultAccess::ownedField({}, &rejecting);
    auto source = ruvia::detail::DbResultAccess::ownedField(
        std::string_view("database field large enough to require an allocation"),
        std::pmr::get_default_resource());
    rejecting.rejectAllocations();

    bool allocationFailure = false;
    try {
        destination = std::move(source);
    } catch (const std::bad_alloc&) {
        allocationFailure = true;
    }
    RUVIA_CHECK(allocationFailure);

    rejecting.rejectAllocations(false);
    auto destinationRow = ruvia::detail::DbResultAccess::ownedRow(&rejecting);
    auto sourceRow = ruvia::detail::DbResultAccess::ownedRow(
        std::pmr::get_default_resource());
    ruvia::detail::DbResultAccess::ownedFields(sourceRow).emplace_back(
        ruvia::detail::DbResultAccess::ownedField(
            "row field",
            std::pmr::get_default_resource()));
    ruvia::detail::DbResultAccess::refresh(sourceRow);
    rejecting.rejectAllocations();

    allocationFailure = false;
    try {
        destinationRow = std::move(sourceRow);
    } catch (const std::bad_alloc&) {
        allocationFailure = true;
    }
    RUVIA_CHECK(allocationFailure);
}
