#include "test_harness.h"

#include <array>
#include <concepts>
#include <cstdint>
#include <initializer_list>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "ruvia/web/db/Db.h"
#include "ruvia/web/detail/db/DbInternal.h"
#include "ruvia/web/detail/db/DbOperationState.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#include "ruvia/web/detail/db/DbValueAccess.h"

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
static_assert(std::is_copy_constructible_v<ruvia::DbValue>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::DbValue>);
static_assert(!std::is_copy_assignable_v<ruvia::DbValue>);
static_assert(!std::is_move_assignable_v<ruvia::DbValue>);

template <typename String>
concept AcceptsTemporaryDbValueText = requires(String&& value) {
    ruvia::DbValue(std::forward<String>(value));
};

template <typename String>
concept AcceptsLvalueDbValueText = requires(String& value) {
    ruvia::DbValue(value);
};

static_assert(!AcceptsTemporaryDbValueText<std::string>);
static_assert(!AcceptsTemporaryDbValueText<const std::string>);
static_assert(AcceptsLvalueDbValueText<std::string>);

template <typename T>
concept ExposesDbValueInspection = requires(const T& value) {
    value.type();
    value.text();
    value.signedValue();
    value.unsignedValue();
    value.doubleValue();
    value.boolValue();
};

static_assert(!ExposesDbValueInspection<ruvia::DbValue>);
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

RUVIA_TEST(database_operation_state_rejects_overlap_and_failed_reuse) {
    struct Lease final {
        int value;
    };

    ruvia::detail::DbOperationState<Lease> state(Lease{7});
    RUVIA_CHECK(state.active());
    auto& lease = state.begin();
    RUVIA_CHECK_EQ(lease.value, 7);
    RUVIA_CHECK(!state.active());

    bool overlapRejected = false;
    try {
        (void)state.begin();
    } catch (const std::logic_error& error) {
        overlapRejected = std::string_view(error.what()) ==
            "database operation is already in progress";
    }
    RUVIA_CHECK(overlapRejected);

    state.finishFailed();
    bool failedReuseRejected = false;
    try {
        (void)state.begin();
    } catch (const std::logic_error& error) {
        failedReuseRejected = std::string_view(error.what()) ==
            "database resource is not active";
    }
    RUVIA_CHECK(failedReuseRejected);
}

RUVIA_TEST(database_cold_operations_do_not_consume_pool_lease) {
    struct Lease final {
        int value;
    };
    auto operate = [](ruvia::detail::DbOperationState<Lease>& state)
        -> ruvia::Task<void> {
        (void)state.begin();
        state.finishActive();
        co_return;
    };

    ruvia::detail::DbOperationState<Lease> state(Lease{7});
    {
        auto cold = operate(state);
        (void)cold;
    }
    RUVIA_CHECK(state.active());
    RUVIA_CHECK_EQ(state.activePayload().value, 7);
}

RUVIA_TEST(db_value_and_result_storage_have_one_live_alternative) {
    const ruvia::DbValue nullValue(nullptr);
    const ruvia::DbValue textValue("value");
    const ruvia::DbValue signedValue(-7);
    const ruvia::DbValue unsignedValue(std::uint64_t{9});
    const ruvia::DbValue doubleValue(1.5);
    const ruvia::DbValue boolValue(true);
    using ValueAccess = ruvia::detail::DbValueAccess;
    RUVIA_CHECK(
        ValueAccess::type(nullValue) == ruvia::detail::DbValueType::kNull);
    RUVIA_CHECK(
        ValueAccess::type(textValue) == ruvia::detail::DbValueType::kString);
    RUVIA_CHECK_EQ(ValueAccess::text(textValue), std::string_view("value"));
    RUVIA_CHECK(
        ValueAccess::type(signedValue) == ruvia::detail::DbValueType::kSigned);
    RUVIA_CHECK_EQ(ValueAccess::signedValue(signedValue), std::int64_t{-7});
    RUVIA_CHECK(
        ValueAccess::type(unsignedValue) ==
        ruvia::detail::DbValueType::kUnsigned);
    RUVIA_CHECK_EQ(
        ValueAccess::unsignedValue(unsignedValue), std::uint64_t{9});
    RUVIA_CHECK(
        ValueAccess::type(doubleValue) == ruvia::detail::DbValueType::kDouble);
    RUVIA_CHECK_EQ(ValueAccess::doubleValue(doubleValue), 1.5);
    RUVIA_CHECK(
        ValueAccess::type(boolValue) == ruvia::detail::DbValueType::kBool);
    RUVIA_CHECK(ValueAccess::boolValue(boolValue));

    auto ownedRow = ruvia::detail::DbResultAccess::ownedRow(nullptr);
    auto& fields = ruvia::detail::DbResultAccess::ownedFields(ownedRow);
    fields.push_back(
        ruvia::detail::DbResultAccess::ownedField("owned", nullptr));
    RUVIA_CHECK_EQ(ownedRow.size(), std::size_t{1});
    RUVIA_CHECK_EQ(ownedRow[0].text(), std::string_view("owned"));

    auto movedRow = std::move(ownedRow);
    RUVIA_CHECK(ownedRow.empty());
    RUVIA_CHECK_EQ(movedRow.size(), std::size_t{1});

    auto borrowedField =
        ruvia::detail::DbResultAccess::borrowedField("borrowed", nullptr);
    auto borrowedRow = ruvia::detail::DbResultAccess::borrowedRow(
        &borrowedField, 1, nullptr);
    RUVIA_CHECK_EQ(borrowedRow[0].text(), std::string_view("borrowed"));

    auto movedField = std::move(borrowedField);
    RUVIA_CHECK(borrowedField.isNull());
    RUVIA_CHECK_EQ(movedField.text(), std::string_view("borrowed"));
}

RUVIA_TEST(db_query_result_move_transfers_direct_raii_ownership) {
    int releases = 0;
    {
        auto result = ruvia::detail::DbResultAccess::makeResult(nullptr);
        ruvia::detail::DbResultAccess::setAffectedRows(result, 7);
        ruvia::detail::DbResultAccess::ownRawResult(
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

RUVIA_TEST(db_registry_derives_default_pool_from_owned_entry_index) {
    asio::io_context ioContext;
#ifdef RUVIA_ENABLE_MARIADB
    const auto config = ruvia::DbConfig::mariaDb();
#else
    const auto config = ruvia::DbConfig::postgreSql();
#endif
    const std::array<ruvia::detail::DbDefinition, 2> definitions{{
        {std::pmr::string("analytics"), config},
        {std::pmr::string("default"), config},
    }};
    ruvia::detail::DbRegistry registry(
        ioContext,
        std::pmr::get_default_resource(),
        definitions);
    ruvia::detail::ScopedOperationScope operationScope;

    bool defaultResolved = true;
    bool aliasResolved = true;
    try {
        (void)registry.get(std::pmr::get_default_resource(), operationScope);
    } catch (...) {
        defaultResolved = false;
    }
    try {
        (void)registry.get(
            "analytics", std::pmr::get_default_resource(), operationScope);
    } catch (...) {
        aliasResolved = false;
    }
    RUVIA_CHECK(defaultResolved);
    RUVIA_CHECK(aliasResolved);
}

RUVIA_TEST(db_handle_copy_rejects_after_parent_scope_closes) {
    asio::io_context ioContext;
#ifdef RUVIA_ENABLE_MARIADB
    const auto config = ruvia::DbConfig::mariaDb();
#else
    const auto config = ruvia::DbConfig::postgreSql();
#endif
    const std::array definitions{
        ruvia::detail::DbDefinition{std::pmr::string("default"), config}};
    ruvia::detail::DbRegistry registry(
        ioContext, std::pmr::get_default_resource(), definitions);
    ruvia::detail::ScopedOperationScope operationScope;
    auto handle = registry.get(
        std::pmr::get_default_resource(), operationScope);
    auto copiedHandle = handle;
    operationScope.close();

    bool handleRejected = false;
    bool copyRejected = false;
    try {
        (void)handle.query("SELECT 1");
    } catch (const std::logic_error&) {
        handleRejected = true;
    }
    try {
        (void)copiedHandle.query("SELECT 1");
    } catch (const std::logic_error&) {
        copyRejected = true;
    }
    RUVIA_CHECK(handleRejected);
    RUVIA_CHECK(copyRejected);
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
    rejecting.rejectAllocations();

    allocationFailure = false;
    try {
        destinationRow = std::move(sourceRow);
    } catch (const std::bad_alloc&) {
        allocationFailure = true;
    }
    RUVIA_CHECK(allocationFailure);
}
