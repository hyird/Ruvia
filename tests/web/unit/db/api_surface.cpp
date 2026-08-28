#include "test_harness.h"

#include <array>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <future>
#include <initializer_list>
#include <memory_resource>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/ip/tcp.hpp>
#include <asio/co_spawn.hpp>
#include <asio/read.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/detail/io/AsioAwait.h"
#include "ruvia/web/db/DbClient.h"
#include "ruvia/web/db/Db.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbPoolOperations.h"
#include "ruvia/web/detail/db/DbPreparedStatement.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbOperationState.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#include "ruvia/web/detail/db/DbSlotSocket.h"
#include "ruvia/web/detail/db/DbValueAccess.h"
#ifdef RUVIA_ENABLE_MARIADB
#include "ruvia/web/detail/db/DbMysqlRuntime.h"
#endif

namespace {

template <typename T>
concept HasMariaDbFactory = requires { T::mariaDb(); };

template <typename T>
concept HasPostgreSqlFactory = requires { T::postgreSql(); };

[[nodiscard]] ruvia::DbConfig testDbConfig() {
#ifdef RUVIA_ENABLE_MARIADB
    return ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb};
#else
    return ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql};
#endif
}

#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)
struct ClosingResolveSlot;

class ClosingResolver final {
public:
    explicit ClosingResolver(ClosingResolveSlot& slot) noexcept
        : slot_(&slot) {}

    template <typename Handler>
    void async_resolve(std::string_view, std::string_view, Handler handler);

    void cancel() noexcept {}

private:
    ClosingResolveSlot* slot_;
};

struct ClosingResolveSlot final {
    enum class DeadlineKind : std::uint8_t { kResolve };

    ClosingResolveSlot()
        : resolver(*this) {}

    bool waitActive{false};
    bool closeRequested{false};
    bool observedActiveResolve{false};
    ClosingResolver resolver;
    ruvia::detail::OperationDeadline<DeadlineKind> deadline;
};

template <typename Handler>
void ClosingResolver::async_resolve(std::string_view, std::string_view, Handler handler) {
    slot_->observedActiveResolve = slot_->waitActive;
    slot_->closeRequested = true;
    handler(asio::error::operation_aborted, asio::ip::tcp::resolver::results_type{});
}

struct ClosingResolvePool final {
    struct Config final {
        std::string host{"resolver.test"};
        std::uint16_t port{3306};
        ruvia::DbDriver driver{ruvia::DbDriver::kMariaDb};
    } config_;

    std::pmr::memory_resource* resource_{std::pmr::get_default_resource()};

    void throwIfCancelled(const ClosingResolveSlot&) const {}

    void clearSlotDeadline(ClosingResolveSlot& slot) noexcept {
        slot.deadline.reset();
    }
};
#endif

[[nodiscard]] ruvia::detail::DbDefinition dbDefinition(std::string_view alias,
    const ruvia::DbConfig& config,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    return {
        std::pmr::string(alias, resource),
        ruvia::detail::DbConfigStorage(config, resource),
    };
}

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

class TrackingResource final : public std::pmr::memory_resource {
public:
    void release() noexcept {
        released_ = true;
    }

    [[nodiscard]] bool deallocatedAfterRelease() const noexcept {
        return deallocatedAfterRelease_;
    }

    [[nodiscard]] std::size_t allocationCount() const noexcept {
        return allocationCount_;
    }

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocationCount_;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        deallocatedAfterRelease_ = deallocatedAfterRelease_ || released_;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }

    bool released_{false};
    bool deallocatedAfterRelease_{false};
    std::size_t allocationCount_{0};
};

template <typename Fn>
bool throwsOn(Fn&& fn) {
    try {
        fn();
        return false;
    } catch (const std::exception&) {
        return true;
    }
}

static_assert(std::is_move_assignable_v<ruvia::DbField>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::DbField>);
static_assert(std::is_move_assignable_v<ruvia::DbRow>);
static_assert(!std::is_nothrow_move_assignable_v<ruvia::DbRow>);
static_assert(std::is_copy_constructible_v<ruvia::DbValue>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::DbValue>);
static_assert(!std::is_copy_assignable_v<ruvia::DbValue>);
static_assert(!std::is_move_assignable_v<ruvia::DbValue>);
static_assert(!std::is_copy_constructible_v<ruvia::DbMigrator>);
static_assert(!std::is_copy_assignable_v<ruvia::DbMigrator>);
static_assert(std::is_nothrow_move_constructible_v<ruvia::DbMigrator>);
static_assert(std::is_nothrow_move_assignable_v<ruvia::DbMigrator>);

template <typename String>
concept AcceptsTemporaryDbValueText =
    requires(String&& value) { ruvia::DbValue(std::forward<String>(value)); };

template <typename String>
concept AcceptsLvalueDbValueText = requires(String& value) { ruvia::DbValue(value); };

template <typename Config>
concept AcceptsValidatedDbConfig =
    requires(Config&& config) { ruvia::detail::validatedDbConfig(std::forward<Config>(config)); };

static_assert(!AcceptsTemporaryDbValueText<std::string>);
static_assert(!AcceptsTemporaryDbValueText<const std::string>);
static_assert(AcceptsLvalueDbValueText<std::string>);
static_assert(AcceptsValidatedDbConfig<ruvia::DbConfig&>);
static_assert(!AcceptsValidatedDbConfig<ruvia::DbConfig>);
static_assert(!AcceptsValidatedDbConfig<const ruvia::DbConfig>);
static_assert(std::constructible_from<ruvia::DbValue, ruvia::BorrowedText>);

template <typename String, typename Migration = ruvia::DbMigration>
concept AcceptsAnyTemporaryDbMigrationText = requires(String&& value) {
    ruvia::DbMigrationOptions{.id = std::forward<String>(value), .sql = "SELECT 1"};
} || requires(String&& value) {
    ruvia::DbMigrationOptions{.id = "migration", .sql = std::forward<String>(value)};
};

template <typename String, typename Migration = ruvia::DbMigration>
concept AcceptsLvalueDbMigrationText =
    requires(String& value) { Migration{{.id = value, .sql = value}}; };

template <typename Migration = ruvia::DbMigration>
concept HasPositionalDbMigrationConstructor =
    requires { Migration(ruvia::BorrowedText{"migration"}, ruvia::BorrowedText{"SELECT 1"}); };

template <typename T>
concept HasDbMigrationTextAccessors = requires(const T& migration) {
    { migration.id() } -> std::same_as<std::string_view>;
    { migration.sql() } -> std::same_as<std::string_view>;
};

static_assert(AcceptsAnyTemporaryDbMigrationText<std::string>);
static_assert(AcceptsAnyTemporaryDbMigrationText<const std::string>);
static_assert(AcceptsLvalueDbMigrationText<std::string>);
static_assert(std::is_aggregate_v<ruvia::DbMigrationOptions>);
static_assert(std::same_as<decltype(ruvia::DbMigrationOptions{}.id), std::string>);
static_assert(std::same_as<decltype(ruvia::DbMigrationOptions{}.sql), std::string>);
static_assert(
    std::same_as<decltype(ruvia::DbMigrationOptions{}.atomicity), ruvia::DbMigrationAtomicity>);
static_assert(!HasPositionalDbMigrationConstructor<ruvia::DbMigration>);
static_assert(HasDbMigrationTextAccessors<ruvia::DbMigration>);

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
static_assert(std::is_move_constructible_v<ruvia::DbRows>);
static_assert(!std::is_move_assignable_v<ruvia::DbRows>);
static_assert(std::is_trivially_copyable_v<ruvia::DbExecResult>);
static_assert(std::is_move_constructible_v<ruvia::DbStreamResult>);
static_assert(!std::is_move_assignable_v<ruvia::DbStreamResult>);
static_assert(std::is_move_constructible_v<ruvia::DbTransaction>);
static_assert(!std::is_move_assignable_v<ruvia::DbTransaction>);

template <typename T>
concept ExposesAnyRvalueDbOwnedView =
    requires(T&& value) { std::move(value).value(); } ||
    requires(T&& value) { std::move(value).template as<std::string_view>(); } ||
    requires(T&& value) { std::move(value)[std::size_t{}]; } ||
    requires(T&& value) { std::move(value).begin(); } ||
    requires(T&& value) { std::move(value).end(); } ||
    requires(T&& value) { std::move(value).applied(); } ||
    requires(T&& value) { std::move(value).skipped(); };

template <typename T>
concept HasLegacyDbRowsAccessor = requires(const T& value) { value.rows(); };

static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::DbValue>);
static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::DbField>);
static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::DbRow>);
static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::DbRows>);
static_assert(!ExposesAnyRvalueDbOwnedView<ruvia::DbMigrationReport>);
static_assert(!HasLegacyDbRowsAccessor<ruvia::DbRows>);

template <typename T>
concept HasDbHandleDefaultParams = requires(const T& handle) {
    handle.query(std::string_view{});
    handle.execute(std::string_view{});
    handle.queryStream(std::string_view{});
};

template <typename T>
concept HasDbHandleSpanParams = requires(const T& handle, std::span<const ruvia::DbValue> params) {
    handle.query(std::string_view{}, params);
    handle.execute(std::string_view{}, params);
    handle.queryStream(std::string_view{}, params);
};

template <typename T>
concept HasDbHandleInitializerListParams =
    requires(const T& handle, std::initializer_list<ruvia::DbValue> params) {
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
concept HasDbTransactionSpanParams =
    requires(T& transaction, std::span<const ruvia::DbValue> params) {
        transaction.query(std::string_view{}, params);
        transaction.execute(std::string_view{}, params);
    };

template <typename T>
concept HasDbTransactionInitializerListParams =
    requires(T& transaction, std::initializer_list<ruvia::DbValue> params) {
        transaction.query(std::string_view{}, params);
        transaction.execute(std::string_view{}, params);
    };

static_assert(HasDbHandleDefaultParams<ruvia::DbHandle>);
static_assert(HasDbHandleSpanParams<ruvia::DbHandle>);
static_assert(!HasDbHandleInitializerListParams<ruvia::DbHandle>);
static_assert(std::constructible_from<ruvia::DbClient, ruvia::EventLoop, ruvia::DbConfig>);
static_assert(!std::copy_constructible<ruvia::DbClient>);
static_assert(!std::move_constructible<ruvia::DbClient>);
static_assert(
    std::same_as<decltype(std::declval<ruvia::DbClient&>().connect()), ruvia::Task<void>>);
static_assert(HasDbHandleDefaultParams<ruvia::DbClient>);
static_assert(HasDbHandleSpanParams<ruvia::DbClient>);
static_assert(!HasDbHandleInitializerListParams<ruvia::DbClient>);
static_assert(std::same_as<decltype(std::declval<const ruvia::DbClient&>().withOptions(
                               ruvia::OperationOptions{})),
    ruvia::DbHandle>);
static_assert(std::same_as<decltype(std::declval<const ruvia::DbClient&>().worker()),
    const ruvia::WorkerHandle&>);
static_assert(std::same_as<decltype(std::declval<const ruvia::DbHandle&>().withOptions(
                               ruvia::OperationOptions{})),
    ruvia::DbHandle>);
static_assert(HasDbTransactionDefaultParams<ruvia::DbTransaction>);
static_assert(HasDbTransactionSpanParams<ruvia::DbTransaction>);
static_assert(!HasDbTransactionInitializerListParams<ruvia::DbTransaction>);

// Bound parameters passed as ordinary arguments.
template <typename T>
concept HasVariadicParams = requires(T& handle) {
    handle.query(std::string_view{}, 1, std::string_view{});
    handle.execute(std::string_view{}, 1, std::string_view{});
};

// A prepared sequence must keep selecting the span overload rather than being
// absorbed as a single bound parameter, which would send the wrong argument.
template <typename T>
concept VariadicParamsRejectSequences =
    !requires(T& handle, std::span<const ruvia::DbValue> params) {
        { handle.query(std::string_view{}, params) } -> std::same_as<void>;
    } && !std::constructible_from<ruvia::DbValue, std::span<const ruvia::DbValue>> &&
    !std::constructible_from<ruvia::DbValue, std::array<ruvia::DbValue, 2>>;

// Variadic calls clone an owning-string temporary before returning, while the
// storable DbValue type above continues to reject the same temporary.
template <typename T>
concept HasVariadicOwningTemporaryParams = requires(T& handle) {
    handle.query(std::string_view{}, std::string("owned"));
    handle.execute(std::string_view{}, std::string("owned"));
};

static_assert(HasVariadicParams<ruvia::DbHandle>);
static_assert(HasVariadicParams<ruvia::DbTransaction>);
static_assert(VariadicParamsRejectSequences<ruvia::DbHandle>);
static_assert(VariadicParamsRejectSequences<ruvia::DbTransaction>);
static_assert(HasVariadicOwningTemporaryParams<ruvia::DbHandle>);
static_assert(HasVariadicOwningTemporaryParams<ruvia::DbTransaction>);

// An lvalue string is fine: it outlives the call, which is all the synchronous
// parameter cloning requires.
template <typename T>
concept HasVariadicOwningLvalueParams =
    requires(T& handle, std::string owned) { handle.query(std::string_view{}, owned); };

static_assert(HasVariadicOwningLvalueParams<ruvia::DbHandle>);
static_assert(HasVariadicOwningLvalueParams<ruvia::DbTransaction>);

}  // namespace

RUVIA_TEST(db_api_surface_uses_span_params_without_initializer_list_overloads) {
    RUVIA_CHECK(true);
}

RUVIA_TEST(db_operation_options_validate_and_compose_restrictions) {
    RUVIA_CHECK(throwsOn([] {
        ruvia::detail::validateOperationOptions(
            ruvia::OperationOptions{.timeout = std::chrono::milliseconds(0)});
    }));
    RUVIA_CHECK(throwsOn([] {
        ruvia::detail::validateOperationOptions(
            ruvia::OperationOptions{.timeout = std::chrono::milliseconds(-1)});
    }));

    ruvia::StopSource ambient;
    ruvia::StopSource explicitOperation;
    auto merged = ruvia::detail::mergeOperationOptions(
        ruvia::OperationOptions{
            .timeout = std::chrono::milliseconds(100), .stopToken = ambient.token()},
        ruvia::OperationOptions{
            .timeout = std::chrono::milliseconds(250), .stopToken = explicitOperation.token()});
    RUVIA_CHECK(merged.timeout == std::chrono::milliseconds(100));
    RUVIA_CHECK(!merged.stopToken.stopRequested());
    explicitOperation.requestStop();
    RUVIA_CHECK(merged.stopToken.stopRequested());

    ruvia::StopSource secondAmbient;
    ruvia::StopSource secondExplicit;
    auto shorterOverride = ruvia::detail::mergeOperationOptions(
        ruvia::OperationOptions{
            .timeout = std::chrono::milliseconds(500), .stopToken = secondAmbient.token()},
        ruvia::OperationOptions{
            .timeout = std::chrono::milliseconds(50), .stopToken = secondExplicit.token()});
    RUVIA_CHECK(shorterOverride.timeout == std::chrono::milliseconds(50));
    secondAmbient.requestStop();
    RUVIA_CHECK(shorterOverride.stopToken.stopRequested());
}

RUVIA_TEST(db_error_carries_category_and_native_diagnostics) {
    const ruvia::DbError timeout(
        ruvia::DbError::Code::kTimeout, ruvia::DbDriver::kMariaDb, "timeout", 1205, "HY000");
    const ruvia::DbError uniqueViolation(ruvia::DbError::Code::kStatementFailed,
        ruvia::DbDriver::kPostgreSql, "duplicate key", std::nullopt, "23505", "uq_jobs_key");
    const ruvia::DbError cancelled(
        ruvia::DbError::Code::kCancelled, ruvia::DbDriver::kPostgreSql, "cancelled");
    const ruvia::DbError closing(
        ruvia::DbError::Code::kClosing, ruvia::DbDriver::kMariaDb, "closing");
    RUVIA_CHECK(timeout.code() == ruvia::DbError::Code::kTimeout);
    RUVIA_CHECK(timeout.driver() == ruvia::DbDriver::kMariaDb);
    RUVIA_CHECK(timeout.nativeCode() == 1205);
    RUVIA_CHECK(timeout.sqlState() == "HY000");
    RUVIA_CHECK(!timeout.constraintName().has_value());
    RUVIA_CHECK(uniqueViolation.constraintName() == "uq_jobs_key");
    RUVIA_CHECK(cancelled.code() == ruvia::DbError::Code::kCancelled);
    RUVIA_CHECK(cancelled.driver() == ruvia::DbDriver::kPostgreSql);
    RUVIA_CHECK(!cancelled.nativeCode().has_value());
    RUVIA_CHECK(!cancelled.sqlState().has_value());
    RUVIA_CHECK(!cancelled.constraintName().has_value());
    RUVIA_CHECK(closing.code() == ruvia::DbError::Code::kClosing);
}

#if defined(RUVIA_ENABLE_MARIADB) || defined(RUVIA_ENABLE_POSTGRESQL)
RUVIA_TEST(db_resolve_shutdown_preserves_slot_until_it_reports_closing) {
    asio::io_context ioContext;
    ClosingResolvePool pool;
    ClosingResolveSlot slot;
    auto future = asio::co_spawn(ioContext,
        ruvia::detail::taskAsAwaitable(ruvia::detail::resolveDbHost(
            pool, slot, ruvia::detail::OperationTimeout(std::nullopt), "test database")),
        asio::use_future);
    ioContext.run();

    bool reportedClosing = false;
    try {
        (void)future.get();
    } catch (const ruvia::DbError& error) {
        reportedClosing = error.code() == ruvia::DbError::Code::kClosing;
    }
    RUVIA_CHECK(slot.observedActiveResolve);
    RUVIA_CHECK(!slot.waitActive);
    RUVIA_CHECK(reportedClosing);
}
#endif

#ifdef RUVIA_ENABLE_MARIADB
RUVIA_TEST(mariadb_wait_deadline_uses_the_earliest_source) {
    using namespace std::chrono_literals;
    using ruvia::detail::MysqlWaitDeadlineSource;

    const auto operationFirst = ruvia::detail::selectMysqlWaitDeadline(30s, 1s);
    RUVIA_CHECK(operationFirst.timeout == 1s);
    RUVIA_CHECK(operationFirst.source == MysqlWaitDeadlineSource::kDriver);

    const auto driverLater = ruvia::detail::selectMysqlWaitDeadline(1s, 30s);
    RUVIA_CHECK(driverLater.timeout == 1s);
    RUVIA_CHECK(driverLater.source == MysqlWaitDeadlineSource::kOperation);

    const auto tie = ruvia::detail::selectMysqlWaitDeadline(1s, 1s);
    RUVIA_CHECK(tie.timeout == 1s);
    RUVIA_CHECK(tie.source == MysqlWaitDeadlineSource::kOperation);

    const auto driverOnly = ruvia::detail::selectMysqlWaitDeadline(std::nullopt, 2s);
    RUVIA_CHECK(driverOnly.timeout == 2s);
    RUVIA_CHECK(driverOnly.source == MysqlWaitDeadlineSource::kDriver);
}
#endif

RUVIA_TEST(db_slot_socket_cancel_drains_before_release_and_preserves_driver_socket) {
    asio::io_context ioContext;
    asio::ip::tcp::acceptor acceptor(
        ioContext, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    asio::ip::tcp::socket driverSocket(ioContext);
    driverSocket.connect(acceptor.local_endpoint());
    asio::ip::tcp::socket peerSocket(ioContext);
    acceptor.accept(peerSocket);

    std::error_code driverReleaseError;
    const auto source = static_cast<ruvia::detail::DbSlotSocket::NativeSocket>(
        driverSocket.release(driverReleaseError));
    RUVIA_CHECK(!driverReleaseError);
    ruvia::detail::DbSlotSocket waitSocket(ioContext);
    RUVIA_CHECK(!waitSocket.ensureAssigned(source));
#if defined(_WIN32)
    RUVIA_CHECK(static_cast<ruvia::detail::DbSlotSocket::NativeSocket>(
                    waitSocket.socket.native_handle()) == source);
#else
    RUVIA_CHECK(waitSocket.descriptor.native_handle() == source);
#endif

    int completions = 0;
    std::error_code waitError;
#if defined(_WIN32)
    waitSocket.socket.async_wait(asio::ip::tcp::socket::wait_read, [&](std::error_code error) {
        ++completions;
        waitError = error;
    });
#else
    waitSocket.descriptor.async_wait(
        asio::posix::stream_descriptor::wait_read, [&](std::error_code error) {
            ++completions;
            waitError = error;
        });
#endif
    waitSocket.cancel();
    ioContext.run();
    RUVIA_CHECK_EQ(completions, 1);
    RUVIA_CHECK(waitError == asio::error::operation_aborted);
    RUVIA_CHECK(!waitSocket.release());

    std::error_code driverAssignError;
    driverSocket.assign(asio::ip::tcp::v4(), source, driverAssignError);
    RUVIA_CHECK(!driverAssignError);
    constexpr std::array<char, 2> payload{'o', 'k'};
    std::array<char, payload.size()> received{};
    asio::write(driverSocket, asio::buffer(payload));
    asio::read(peerSocket, asio::buffer(received));
    RUVIA_CHECK(received == payload);
}

RUVIA_TEST(db_slot_socket_reports_invalid_driver_socket) {
    asio::io_context ioContext;
    ruvia::detail::DbSlotSocket waitSocket(ioContext);
    const auto error = waitSocket.ensureAssigned(ruvia::detail::DbSlotSocket::kInvalidSocket);
    RUVIA_CHECK(error == std::errc::bad_file_descriptor);
}

RUVIA_TEST(db_slot_socket_releases_before_driver_socket_closes) {
    asio::io_context ioContext;
    asio::ip::tcp::acceptor acceptor(
        ioContext, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    asio::ip::tcp::socket driverSocket(ioContext);
    driverSocket.connect(acceptor.local_endpoint());
    asio::ip::tcp::socket peerSocket(ioContext);
    acceptor.accept(peerSocket);

    std::error_code driverReleaseError;
    const auto source = static_cast<ruvia::detail::DbSlotSocket::NativeSocket>(
        driverSocket.release(driverReleaseError));
    RUVIA_CHECK(!driverReleaseError);
    {
        ruvia::detail::DbSlotSocket waitSocket(ioContext);
        RUVIA_CHECK(!waitSocket.ensureAssigned(source));
#if defined(_WIN32)
        RUVIA_CHECK(static_cast<ruvia::detail::DbSlotSocket::NativeSocket>(
                        waitSocket.socket.native_handle()) == source);
#else
        RUVIA_CHECK(waitSocket.descriptor.native_handle() == source);
#endif
        RUVIA_CHECK(!waitSocket.release());
        std::error_code driverAssignError;
        driverSocket.assign(asio::ip::tcp::v4(), source, driverAssignError);
        RUVIA_CHECK(!driverAssignError);
        std::error_code closeError;
        driverSocket.close(closeError);
        RUVIA_CHECK(!closeError);
    }

    std::array<char, 1> byte{};
    std::error_code readError;
    (void)peerSocket.read_some(asio::buffer(byte), readError);
    RUVIA_CHECK(readError == asio::error::eof || readError == asio::error::connection_reset);
}

RUVIA_TEST(db_api_surface_accepts_variadic_params_without_absorbing_sequences) {
    RUVIA_CHECK(true);
}

RUVIA_TEST(db_prepared_statement_rejects_blank_sql_before_io) {
    RUVIA_CHECK(throwsOn(
        [] { (void)ruvia::prepareDbStatement("", {}, std::pmr::get_default_resource()); }));
    RUVIA_CHECK(throwsOn(
        [] { (void)ruvia::prepareDbStatement(" \n\t\r", {}, std::pmr::get_default_resource()); }));

    const auto statement =
        ruvia::prepareDbStatement("SELECT 1", {}, std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(std::string_view(statement.sql), std::string_view("SELECT 1"));
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
        overlapRejected =
            std::string_view(error.what()) == "database operation is already in progress";
    }
    RUVIA_CHECK(overlapRejected);

    state.finishFailed();
    bool failedReuseRejected = false;
    try {
        (void)state.begin();
    } catch (const std::logic_error& error) {
        failedReuseRejected = std::string_view(error.what()) == "database resource is not active";
    }
    RUVIA_CHECK(failedReuseRejected);
}

RUVIA_TEST(database_cold_operations_do_not_consume_pool_lease) {
    struct Lease final {
        int value;
    };
    auto operate = [](ruvia::detail::DbOperationState<Lease>& state) -> ruvia::Task<void> {
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

RUVIA_TEST(scoped_operation_scope_tracks_cold_owner_operations) {
    ruvia::detail::ScopedOperationScope operationScope;
    auto coldTask = []() -> ruvia::Task<void> { co_return; }();
    {
        auto operation = ruvia::detail::makeScopedOperation(operationScope, std::move(coldTask));
        RUVIA_CHECK(operationScope.hasPendingOperations());
    }
    RUVIA_CHECK(!operationScope.hasPendingOperations());
}

RUVIA_TEST(db_value_and_result_storage_have_one_live_alternative) {
    const ruvia::DbValue nullValue(nullptr);
    const ruvia::DbValue textValue("value");
    const ruvia::DbValue borrowedTextValue(ruvia::BorrowedText("borrowed-value"));
    const ruvia::DbValue signedValue(-7);
    const ruvia::DbValue unsignedValue(std::uint64_t{9});
    const ruvia::DbValue doubleValue(1.5);
    const ruvia::DbValue boolValue(true);
    using ValueAccess = ruvia::detail::DbValueAccess;
    RUVIA_CHECK(ValueAccess::type(nullValue) == ruvia::detail::DbValueType::kNull);
    RUVIA_CHECK(ValueAccess::type(textValue) == ruvia::detail::DbValueType::kString);
    RUVIA_CHECK_EQ(ValueAccess::text(textValue), std::string_view("value"));
    RUVIA_CHECK(ValueAccess::type(borrowedTextValue) == ruvia::detail::DbValueType::kString);
    RUVIA_CHECK_EQ(ValueAccess::text(borrowedTextValue), std::string_view("borrowed-value"));
    RUVIA_CHECK(ValueAccess::type(signedValue) == ruvia::detail::DbValueType::kSigned);
    RUVIA_CHECK_EQ(ValueAccess::signedValue(signedValue), std::int64_t{-7});
    RUVIA_CHECK(ValueAccess::type(unsignedValue) == ruvia::detail::DbValueType::kUnsigned);
    RUVIA_CHECK_EQ(ValueAccess::unsignedValue(unsignedValue), std::uint64_t{9});
    RUVIA_CHECK(ValueAccess::type(doubleValue) == ruvia::detail::DbValueType::kDouble);
    RUVIA_CHECK_EQ(ValueAccess::doubleValue(doubleValue), 1.5);
    RUVIA_CHECK(ValueAccess::type(boolValue) == ruvia::detail::DbValueType::kBool);
    RUVIA_CHECK(ValueAccess::boolValue(boolValue));

    auto ownedRow = ruvia::detail::DbResultAccess::ownedRow(nullptr);
    auto& fields = ruvia::detail::DbResultAccess::ownedFields(ownedRow);
    auto& columnNames = ruvia::detail::DbResultAccess::ownedColumnNames(ownedRow);
    columnNames.emplace_back("label");
    fields.push_back(ruvia::detail::DbResultAccess::ownedField("owned", nullptr));
    RUVIA_CHECK_EQ(ownedRow.size(), std::size_t{1});
    RUVIA_CHECK(ownedRow[0].value() == std::optional<std::string_view>("owned"));
    RUVIA_CHECK(ownedRow["label"].as<std::string>() == std::optional<std::string>("owned"));

    auto movedRow = std::move(ownedRow);
    RUVIA_CHECK(ownedRow.empty());
    RUVIA_CHECK_EQ(movedRow.size(), std::size_t{1});

    auto borrowedField = ruvia::detail::DbResultAccess::borrowedField("borrowed", nullptr);
    const std::pmr::string borrowedColumn("borrowed_column");
    auto borrowedRow =
        ruvia::detail::DbResultAccess::borrowedRow(&borrowedField, 1, &borrowedColumn, 1, nullptr);
    RUVIA_CHECK(borrowedRow["borrowed_column"].as<std::string_view>() ==
                std::optional<std::string_view>("borrowed"));

    auto movedField = std::move(borrowedField);
    // DbField defines an observable empty moved-from state; this assertion is
    // the contract under test rather than an accidental post-move use.
    RUVIA_CHECK(!borrowedField.value().has_value());  // NOLINT(clang-analyzer-cplusplus.Move)
    RUVIA_CHECK(movedField.value() == std::optional<std::string_view>("borrowed"));
    auto numeric = ruvia::detail::DbResultAccess::ownedField("-42", nullptr);
    RUVIA_CHECK(numeric.as<std::int64_t>() == std::optional<std::int64_t>(-42));
    auto boolean = ruvia::detail::DbResultAccess::ownedField("t", nullptr);
    RUVIA_CHECK(boolean.as<bool>() == std::optional<bool>(true));
    auto null = ruvia::detail::DbResultAccess::nullField(nullptr);
    RUVIA_CHECK(!null.as<std::int64_t>().has_value());
    auto invalid = ruvia::detail::DbResultAccess::ownedField("not-a-number", nullptr);
    try {
        (void)invalid.as<std::int64_t>();
        RUVIA_CHECK(false);
    } catch (const ruvia::DbConversionError& error) {
        RUVIA_CHECK(error.code() == ruvia::DbConversionError::Code::kInvalidFormat);
    }

    try {
        (void)movedRow["missing"];
        RUVIA_CHECK(false);
    } catch (const std::out_of_range&) {
    }
}

RUVIA_TEST(db_query_rows_and_execution_metadata_have_independent_storage) {
    int releases = 0;
    {
        auto result = ruvia::detail::DbResultAccess::makeResult(nullptr);
        auto& columnNames = ruvia::detail::DbResultAccess::columnNames(result);
        auto& fields = ruvia::detail::DbResultAccess::fields(result);
        auto& rows = ruvia::detail::DbResultAccess::rows(result);
        columnNames.emplace_back("value");
        fields.push_back(ruvia::detail::DbResultAccess::borrowedField("stable", nullptr));
        rows.push_back(ruvia::detail::DbResultAccess::borrowedRow(
            fields.data(), fields.size(), columnNames.data(), columnNames.size(), nullptr));
        ruvia::detail::DbResultAccess::ownRawResult(
            result, &releases, [](void* value) noexcept { ++*static_cast<int*>(value); });
        const auto execution = ruvia::detail::DbResultAccess::makeExecResult(7);

        auto moved = std::move(result);
        RUVIA_CHECK_EQ(execution.affectedRows(), std::uint64_t{7});
        RUVIA_CHECK(!execution.lastInsertId().has_value());
        RUVIA_CHECK_EQ(moved.size(), std::size_t{1});
        RUVIA_CHECK(moved[0]["value"].value() == std::optional<std::string_view>("stable"));
        RUVIA_CHECK_EQ(releases, 0);
    }
    RUVIA_CHECK_EQ(releases, 1);
}

RUVIA_TEST(db_registry_derives_default_pool_from_owned_entry_index) {
    asio::io_context ioContext;
#ifdef RUVIA_ENABLE_MARIADB
    const auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb};
#else
    const auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql};
#endif
    const std::array<ruvia::detail::DbDefinition, 2> definitions{{
        dbDefinition("analytics", config),
        dbDefinition("default", config),
    }};
    ruvia::detail::DbRegistry registry(ioContext, std::pmr::get_default_resource(), definitions);
    ruvia::detail::ScopedOperationScope operationScope;

    bool defaultResolved = true;
    bool aliasResolved = true;
    try {
        (void)registry.get(std::pmr::get_default_resource(), operationScope);
    } catch (...) {
        defaultResolved = false;
    }
    try {
        (void)registry.get("analytics", std::pmr::get_default_resource(), operationScope);
    } catch (...) {
        aliasResolved = false;
    }
    RUVIA_CHECK(defaultResolved);
    RUVIA_CHECK(aliasResolved);
}

RUVIA_TEST(db_registry_reports_typed_not_configured_error) {
    asio::io_context ioContext;
    ruvia::detail::DbRegistry registry(ioContext, std::pmr::get_default_resource(),
        std::span<const ruvia::detail::DbDefinition>());
    ruvia::detail::ScopedOperationScope operationScope;

    bool defaultTyped = false;
    bool aliasTyped = false;
    try {
        (void)registry.get(std::pmr::get_default_resource(), operationScope);
    } catch (const ruvia::DbError& error) {
        defaultTyped =
            error.code() == ruvia::DbError::Code::kNotConfigured && !error.driver().has_value();
    }
    try {
        (void)registry.get("missing", std::pmr::get_default_resource(), operationScope);
    } catch (const ruvia::DbError& error) {
        aliasTyped =
            error.code() == ruvia::DbError::Code::kNotConfigured && !error.driver().has_value();
    }
    RUVIA_CHECK(defaultTyped);
    RUVIA_CHECK(aliasTyped);
}

RUVIA_TEST(db_config_is_direct_aggregate_without_factories) {
    static_assert(std::default_initializable<ruvia::DbConfig>);
    static_assert(std::is_aggregate_v<ruvia::DbConfig>);
    static_assert(
        std::same_as<decltype(std::declval<const ruvia::DbConfig&>().driver), ruvia::DbDriver>);
    static_assert(!HasMariaDbFactory<ruvia::DbConfig>);
    static_assert(!HasPostgreSqlFactory<ruvia::DbConfig>);
    RUVIA_CHECK(true);
}

RUVIA_TEST(db_registry_owns_nested_pmr_configuration) {
    TrackingResource sourceResource;
    std::pmr::unsynchronized_pool_resource targetResource;
    asio::io_context ioContext;
    std::optional<ruvia::detail::DbDefinition> definition;
    auto config = testDbConfig();
    config.host = std::string(80, 'h');
    config.username = std::string(80, 'u');
    config.password = std::string(80, 'p');
    config.database = std::string(80, 'd');
    definition.emplace(dbDefinition("default", config, &sourceResource));

    std::optional<ruvia::detail::DbRegistry> registry;
    registry.emplace(
        ioContext, &targetResource, std::span<const ruvia::detail::DbDefinition>(&*definition, 1));
    definition.reset();
    sourceResource.release();
    registry.reset();

    RUVIA_CHECK(!sourceResource.deallocatedAfterRelease());
}

RUVIA_TEST(db_handle_copy_rejects_after_parent_scope_closes) {
    asio::io_context ioContext;
#ifdef RUVIA_ENABLE_MARIADB
    const auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb};
#else
    const auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql};
#endif
    const std::array definitions{dbDefinition("default", config)};
    ruvia::detail::DbRegistry registry(ioContext, std::pmr::get_default_resource(), definitions);
    ruvia::detail::ScopedOperationScope operationScope;
    auto handle = registry.get(std::pmr::get_default_resource(), operationScope);
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
        ruvia::DbMigration{{.id = "duplicate", .sql = "SELECT 1"}},
        ruvia::DbMigration{{.id = "duplicate", .sql = "SELECT 2"}},
    }};
    bool rejected = false;
    try {
        (void)ruvia::DbMigrator::migrate(
            testDbConfig(), std::span<const ruvia::DbMigration>(migrations));
    } catch (const std::invalid_argument& error) {
        rejected = std::string_view(error.what()) ==
                   "database migration ids must be unique, including case";
    }
    RUVIA_CHECK(rejected);
}

#ifdef RUVIA_ENABLE_POSTGRESQL
RUVIA_TEST(db_migrator_rejects_unrepresentable_postgresql_lock_timeout_before_connecting) {
    auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql};
    ruvia::DbMigratorOptions options;
    options.lockTimeout = std::chrono::seconds::max();

    bool rejected = false;
    try {
        (void)ruvia::DbMigrator::migrate(config, std::span<const ruvia::DbMigration>(), options);
    } catch (const std::invalid_argument& error) {
        rejected =
            std::string_view(error.what()) ==
            "database migration lock timeout cannot be represented as PostgreSQL milliseconds";
    }
    RUVIA_CHECK(rejected);
}
#endif

RUVIA_TEST(db_migrator_copies_public_configuration) {
    TrackingResource targetResource;
    std::optional<ruvia::DbMigrator> migrator;
    {
        auto config = testDbConfig();
        config.host = std::string(80, 'h');
        config.username = std::string(80, 'u');
        config.password = std::string(80, 'p');
        config.database = std::string(80, 'd');
        ruvia::DbMigratorOptions options{
            .table = std::string(60, 't'),
            .lockTimeout = std::chrono::seconds(30),
            .resource = &targetResource,
        };
        migrator.emplace(config, options);
    }
    // Opaque owner + four long DB strings + the long migration table all use
    // the caller's resource rather than their public std::string allocators.
    RUVIA_CHECK(targetResource.allocationCount() >= 6);
    migrator.reset();
    RUVIA_CHECK(true);
}

RUVIA_TEST(db_migrator_validates_complete_configuration_before_allocating) {
    {
        auto config = testDbConfig();
        config.host = std::string(80, 'h');
        config.connectTimeout = std::chrono::milliseconds::zero();
        TrackingResource resource;
        const ruvia::DbMigratorOptions options{
            .table = std::string(60, 't'),
            .resource = &resource,
        };

        RUVIA_CHECK(throwsOn([&] { (void)ruvia::DbMigrator(config, options); }));
        RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});
    }
    {
        auto config = testDbConfig();
        config.host = std::string(80, 'h');
        TrackingResource resource;
        const ruvia::DbMigratorOptions options{
            .table = "invalid-table-name",
            .resource = &resource,
        };

        RUVIA_CHECK(throwsOn([&] { (void)ruvia::DbMigrator(config, options); }));
        RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});
    }
}

RUVIA_TEST(db_migrator_validates_migration_list_before_allocating_runtime) {
    const std::array<ruvia::DbMigration, 2> migrations{{
        ruvia::DbMigration{{.id = "duplicate", .sql = "SELECT 1"}},
        ruvia::DbMigration{{.id = "duplicate", .sql = "SELECT 2"}},
    }};
    auto config = testDbConfig();
    config.host = std::string(80, 'h');
    TrackingResource resource;
    const ruvia::DbMigratorOptions options{
        .table = std::string(60, 't'),
        .resource = &resource,
    };

    RUVIA_CHECK(throwsOn([&] {
        (void)ruvia::DbMigrator::migrate(
            config, std::span<const ruvia::DbMigration>(migrations), options);
    }));
    RUVIA_CHECK_EQ(resource.allocationCount(), std::size_t{0});
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
    auto sourceRow = ruvia::detail::DbResultAccess::ownedRow(std::pmr::get_default_resource());
    ruvia::detail::DbResultAccess::ownedFields(sourceRow).emplace_back(
        ruvia::detail::DbResultAccess::ownedField("row field", std::pmr::get_default_resource()));
    rejecting.rejectAllocations();

    allocationFailure = false;
    try {
        destinationRow = std::move(sourceRow);
    } catch (const std::bad_alloc&) {
        allocationFailure = true;
    }
    RUVIA_CHECK(allocationFailure);
}
