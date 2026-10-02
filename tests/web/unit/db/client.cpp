#include <array>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstdint>
#include <future>
#include <initializer_list>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/read.hpp>
#include <asio/use_future.hpp>
#include <asio/write.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/web/db/Db.h"
#include "ruvia/web/detail/db/DbConfigValidation.h"
#include "ruvia/web/detail/db/DbOperationState.h"
#include "ruvia/web/detail/db/DbPoolOperations.h"
#include "ruvia/web/detail/db/DbPreparedStatement.h"
#include "ruvia/web/detail/db/DbRegistry.h"
#include "ruvia/web/detail/db/DbResultAccess.h"
#include "ruvia/web/detail/db/DbSlotSocket.h"
#include "ruvia/web/detail/db/DbValueAccess.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"
#ifdef RUVIA_ENABLE_MARIADB
#include "ruvia/web/detail/db/DbMysqlRuntime.h"
#endif

namespace {

using ruvia::test::RejectingMemoryResource;
using ruvia::test::TrackingResource;
using ruvia::testing::throwsOn;

std::size_t owned_string_allocations() {
    ruvia::test::CountingMemoryResource memory;
    // Include implementation-owned storage such as MSVC debug iterator proxies.
    const std::pmr::string input(256, 'x', &memory);
    return memory.liveAllocations();
}

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

    void setSlotDeadline(ClosingResolveSlot& slot, std::chrono::milliseconds timeout,
        ClosingResolveSlot::DeadlineKind kind) {
        slot.deadline.arm(ruvia::detail::workerTimerDeadlineAfter(timeout), kind);
    }

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

using GuardedLease = std::pmr::string;
using GuardedLeaseState = ruvia::detail::DbOperationState<GuardedLease>;
using GuardedLeaseGuard = ruvia::detail::DbOperationGuard<GuardedLease>;

struct GuardedLeaseGate final {
    [[nodiscard]] bool await_ready() const noexcept {
        return false;
    }
    void await_suspend(std::coroutine_handle<> continuation) noexcept {
        continuation_ = continuation;
    }
    void await_resume() const noexcept {}
    void resume() noexcept {
        auto continuation = std::exchange(continuation_, {});
        continuation.resume();
    }
    std::coroutine_handle<> continuation_{};
};

class GuardedLeaseCapability final : public ruvia::detail::ScopedCapabilityNode {
public:
    GuardedLeaseCapability(ruvia::detail::ScopedOperationScope& scope, GuardedLeaseState& state,
        bool& expired) noexcept
        : ScopedCapabilityNode(scope, &GuardedLeaseCapability::expire),
          state_(state),
          expired_(expired) {}

private:
    static void expire(ruvia::detail::ScopedCapabilityNode& node) noexcept {
        auto& capability = static_cast<GuardedLeaseCapability&>(node);
        capability.state_.reset([](GuardedLease&) noexcept {});
        capability.expired_ = true;
    }

    GuardedLeaseState& state_;
    bool& expired_;
};

ruvia::Task<void> failGuardedLeaseAfterGate(
    GuardedLeaseGuard pending, GuardedLeaseGate& gate, std::pmr::string input) {
    GuardedLeaseGuard operation(std::move(pending));
    operation.start();
    co_await gate;
    (void)input;
    throw std::runtime_error("scoped database operation failed");
}

ruvia::Task<void> completeGuardedLease(GuardedLeaseGuard pending, std::pmr::string input) {
    GuardedLeaseGuard operation(std::move(pending));
    operation.start();
    (void)input;
    operation.finishActive();
    co_return;
}

ruvia::Task<void> awaitScopedOperation(ruvia::ScopedOperation<void>& operation) {
    co_await std::move(operation);
    co_return;
}

ruvia::Task<void> joinScopedOperations(ruvia::detail::ScopedOperationScope& scope) {
    co_await scope.closeAndJoin();
    co_return;
}

class DbRegistryTestRuntime final {
#ifndef _WIN32
    asio::io_context ownedIoContext;
#endif

public:
#ifdef _WIN32
    DbRegistryTestRuntime()
        : ioContext(ruvia::test::newTestIoContext()),
          attachment(ruvia::attachEventLoop(ioContext)),
          worker(attachment.loop().handle()) {}
#else
    DbRegistryTestRuntime()
        : ioContext(ownedIoContext),
          attachment(ruvia::attachEventLoop(ioContext)),
          worker(attachment.loop().handle()) {}
#endif

    asio::io_context& ioContext;
    ruvia::EventLoopAttachment attachment;
    ruvia::WorkerHandle worker;
};

// Bound parameters passed as ordinary arguments.

// A prepared sequence must keep selecting the span overload rather than being
// absorbed as a single bound parameter, which would send the wrong argument.

// Variadic calls clone an owning-string temporary before returning, while the
// storable DbValue type above continues to reject the same temporary.

// An lvalue string is fine: it outlives the call, which is all the synchronous
// parameter cloning requires.

}  // namespace

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
        ruvia::asAwaitable(ruvia::detail::resolveDbHost(
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

RUVIA_TEST(db_prepared_statement_rejects_blank_sql_before_io) {
    RUVIA_CHECK(throwsOn(
        [] { (void)ruvia::prepareDbStatement("", {}, std::pmr::get_default_resource()); }));
    RUVIA_CHECK(throwsOn(
        [] { (void)ruvia::prepareDbStatement(" \n\t\r", {}, std::pmr::get_default_resource()); }));

    const auto statement =
        ruvia::prepareDbStatement("SELECT 1", {}, std::pmr::get_default_resource());
    RUVIA_CHECK_EQ(std::string_view(statement.sql), std::string_view("SELECT 1"));
}

RUVIA_TEST(database_operation_state_cold_borrows_and_start_exclusivity) {
    struct Lease final {
        int value;
    };
    using State = ruvia::detail::DbOperationState<Lease>;
    using Guard = ruvia::detail::DbOperationGuard<Lease>;

    State state(Lease{7});
    Guard first(state);
    Guard second(state);
    RUVIA_CHECK(state.active());

    first.start();
    RUVIA_CHECK_EQ(first.lease().value, 7);
    bool coldLeaseRejected = false;
    try {
        (void)second.lease();
    } catch (const std::logic_error& error) {
        coldLeaseRejected = std::string_view(error.what()) == "database operation is already in progress";
    }
    RUVIA_CHECK(coldLeaseRejected);
    bool overlapRejected = false;
    try {
        second.start();
    } catch (const std::logic_error& error) {
        overlapRejected = std::string_view(error.what()) == "database operation is already in progress";
    }
    RUVIA_CHECK(overlapRejected);
    RUVIA_CHECK_EQ(first.lease().value, 7);
    first.finishActive();

    second.start();
    RUVIA_CHECK_EQ(second.lease().value, 7);
    second.finishActive();
    RUVIA_CHECK(state.active());
}

RUVIA_TEST(database_operation_guard_drops_cold_borrows_without_claiming_lease) {
    struct Payload final {
        int value;
    };
    using State = ruvia::detail::DbOperationState<Payload>;
    using Guard = ruvia::detail::DbOperationGuard<Payload>;

    State state(Payload{7});
    {
        Guard first(state);
        Guard second(state);
        RUVIA_CHECK(state.active());
    }
    RUVIA_CHECK(state.active());

    Guard pending(state);
    Guard failing(state);
    failing.start();
    failing.finishFailed();
    bool failedLeaseRejected = false;
    try {
        (void)pending.lease();
    } catch (const std::logic_error& error) {
        failedLeaseRejected = std::string_view(error.what()) == "database resource is not active";
    }
    RUVIA_CHECK(failedLeaseRejected);
    bool failedStartRejected = false;
    try {
        pending.start();
    } catch (const std::logic_error& error) {
        failedStartRejected = std::string_view(error.what()) == "database resource is not active";
    }
    RUVIA_CHECK(failedStartRejected);

    State closedState(Payload{9});
    Guard closedPending(closedState);
    Guard closer(closedState);
    closer.start();
    closer.finishClosed();
    bool closedLeaseRejected = false;
    try {
        (void)closedPending.lease();
    } catch (const std::logic_error& error) {
        closedLeaseRejected = std::string_view(error.what()) == "database resource is not active";
    }
    RUVIA_CHECK(closedLeaseRejected);
    bool closedStartRejected = false;
    try {
        closedPending.start();
    } catch (const std::logic_error& error) {
        closedStartRejected = std::string_view(error.what()) == "database resource is not active";
    }
    RUVIA_CHECK(closedStartRejected);
}

RUVIA_TEST(database_operation_guard_runs_cold_operation) {
    struct Payload final {
        int value;
    };
    struct Owner final {
        ruvia::detail::DbOperationState<Payload> state{Payload{7}};
    };
    using Guard = ruvia::detail::DbOperationGuard<Payload>;
    auto operate = [](Guard operation, int& observedValue) -> ruvia::Task<void> {
        operation.start();
        observedValue = operation.lease().value;
        operation.finishActive();
        co_return;
    };

    Owner owner;
    int observedValue = 0;
    {
        Guard coldOne(owner.state);
        Guard coldTwo(owner.state);
        auto one = operate(std::move(coldOne), observedValue);
        auto two = operate(std::move(coldTwo), observedValue);
        RUVIA_CHECK(owner.state.active());

        asio::io_context io(1);
        auto first = asio::co_spawn(io, ruvia::asAwaitable(std::move(one)), asio::use_future);
        io.run();
        first.get();
        io.restart();
        auto second = asio::co_spawn(io, ruvia::asAwaitable(std::move(two)), asio::use_future);
        io.run();
        second.get();
    }
    RUVIA_CHECK(owner.state.active());
    RUVIA_CHECK_EQ(observedValue, 7);
}

RUVIA_TEST(database_operation_guarded_cold_tasks_release_owned_inputs_and_retain_results) {
    using Lease = std::pmr::string;
    using State = ruvia::detail::DbOperationState<Lease>;
    using Guard = ruvia::detail::DbOperationGuard<Lease>;
    ruvia::test::CountingMemoryResource memory;

    auto operate = [](Guard operation, std::pmr::string input, std::pmr::memory_resource* resource)
        -> ruvia::Task<std::pmr::string> {
        operation.start();
        std::pmr::string result(input, resource);
        operation.finishActive();
        co_return result;
    };

    State state(Lease("lease"));
    {
        std::optional<std::pmr::string> firstResult;
        std::optional<std::pmr::string> secondResult;
        auto first = operate(Guard(state), std::pmr::string(256, 'a', &memory), &memory);
        auto second = operate(Guard(state), std::pmr::string(256, 'b', &memory), &memory);
        RUVIA_CHECK_EQ(memory.liveAllocations(), 2U * owned_string_allocations());
        {
            auto dropped = operate(Guard(state), std::pmr::string(256, 'x', &memory), &memory);
            RUVIA_CHECK_EQ(memory.liveAllocations(), 3U * owned_string_allocations());
        }
        RUVIA_CHECK_EQ(memory.liveAllocations(), 2U * owned_string_allocations());
        RUVIA_CHECK(state.active());

        asio::io_context io(1);
        {
            auto firstFuture = asio::co_spawn(io, ruvia::asAwaitable(std::move(first)), asio::use_future);
            io.run();
            firstResult.emplace(firstFuture.get(), &memory);
        }
        RUVIA_CHECK_EQ(firstResult->size(), 256U);
        RUVIA_CHECK_EQ(firstResult->front(), 'a');
        RUVIA_CHECK_EQ(firstResult->back(), 'a');
        RUVIA_CHECK_EQ(memory.liveAllocations(), 2U * owned_string_allocations());

        io.restart();
        {
            auto secondFuture = asio::co_spawn(io, ruvia::asAwaitable(std::move(second)), asio::use_future);
            io.run();
            secondResult.emplace(secondFuture.get(), &memory);
        }
        RUVIA_CHECK_EQ(secondResult->size(), 256U);
        RUVIA_CHECK_EQ(secondResult->front(), 'b');
        RUVIA_CHECK_EQ(secondResult->back(), 'b');
        RUVIA_CHECK_EQ(memory.liveAllocations(), 2U * owned_string_allocations());
        RUVIA_CHECK(state.active());
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), 0U);
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(database_operation_guarded_overlapping_task_does_not_damage_first) {
    using Lease = std::pmr::string;
    using State = ruvia::detail::DbOperationState<Lease>;
    using Guard = ruvia::detail::DbOperationGuard<Lease>;
    struct Gate final {
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }
        void await_suspend(std::coroutine_handle<> continuation) noexcept {
            continuation_ = continuation;
        }
        void await_resume() const noexcept {}
        void resume() noexcept {
            auto continuation = std::exchange(continuation_, {});
            continuation.resume();
        }
        std::coroutine_handle<> continuation_{};
    };
    ruvia::test::CountingMemoryResource memory;
    auto operate = [](Guard operation, Gate& gate, std::pmr::string input,
                       std::pmr::memory_resource* resource) -> ruvia::Task<std::pmr::string> {
        operation.start();
        co_await gate;
        std::pmr::string result(input, resource);
        operation.finishActive();
        co_return result;
    };

    State state(Lease("lease"));
    {
        Gate gate;
        auto first = operate(Guard(state), gate, std::pmr::string(256, 'a', &memory), &memory);
        auto second = operate(Guard(state), gate, std::pmr::string(256, 'b', &memory), &memory);
        asio::io_context io(1);
        auto firstFuture = asio::co_spawn(io, ruvia::asAwaitable(std::move(first)), asio::use_future);
        io.poll();
        RUVIA_CHECK(gate.continuation_ != nullptr);

        auto secondFuture = asio::co_spawn(io, ruvia::asAwaitable(std::move(second)), asio::use_future);
        io.restart();
        // The first co_spawn still owns work while suspended at the gate.
        // Only drain ready handlers for the rejected overlapping operation.
        io.poll();
        bool overlapRejected = false;
        try {
            (void)secondFuture.get();
        } catch (const std::logic_error& error) {
            overlapRejected = std::string_view(error.what()) == "database operation is already in progress";
        }
        RUVIA_CHECK(overlapRejected);
        RUVIA_CHECK_EQ(memory.liveAllocations(), owned_string_allocations());

        io.restart();
        gate.resume();
        io.run();
        const auto result = firstFuture.get();
        RUVIA_CHECK_EQ(result.size(), 256U);
        RUVIA_CHECK_EQ(result.front(), 'a');
        RUVIA_CHECK(state.active());
    }
    RUVIA_CHECK_EQ(memory.liveAllocations(), 0U);
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(database_operation_guarded_failure_rejects_pending_task_and_releases_inputs) {
    using Lease = std::pmr::string;
    using State = ruvia::detail::DbOperationState<Lease>;
    using Guard = ruvia::detail::DbOperationGuard<Lease>;
    ruvia::test::CountingMemoryResource memory;

    auto fail = [](Guard operation, std::pmr::string input) -> ruvia::Task<void> {
        operation.start();
        (void)input;
        throw std::runtime_error("operation failed");
        co_return;
    };
    auto complete = [](Guard operation, std::pmr::string input) -> ruvia::Task<void> {
        operation.start();
        (void)input;
        operation.finishActive();
        co_return;
    };

    State state(Lease("lease"));
    auto failed = fail(Guard(state), std::pmr::string(256, 'f', &memory));
    auto pending = complete(Guard(state), std::pmr::string(256, 'p', &memory));
    RUVIA_CHECK_EQ(memory.liveAllocations(), 2U * owned_string_allocations());

    asio::io_context io(1);
    auto failedFuture = asio::co_spawn(io, ruvia::asAwaitable(std::move(failed)), asio::use_future);
    io.run();
    bool failureObserved = false;
    try {
        failedFuture.get();
    } catch (const std::runtime_error& error) {
        failureObserved = std::string_view(error.what()) == "operation failed";
    }
    RUVIA_CHECK(failureObserved);
    RUVIA_CHECK_EQ(memory.liveAllocations(), owned_string_allocations());

    io.restart();
    auto pendingFuture = asio::co_spawn(io, ruvia::asAwaitable(std::move(pending)), asio::use_future);
    io.run();
    bool pendingRejected = false;
    try {
        pendingFuture.get();
    } catch (const std::logic_error& error) {
        pendingRejected = std::string_view(error.what()) == "database resource is not active";
    }
    RUVIA_CHECK(pendingRejected);
    RUVIA_CHECK_EQ(memory.liveAllocations(), 0U);
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(database_operation_guarded_started_cancellation_fails_lease_and_releases_inputs) {
    using Lease = std::pmr::string;
    using State = ruvia::detail::DbOperationState<Lease>;
    using Guard = ruvia::detail::DbOperationGuard<Lease>;
    ruvia::test::CountingMemoryResource memory;
    auto cancel = [](Guard operation, std::pmr::string input) -> ruvia::Task<void> {
        operation.start();
        (void)input;
        throw ruvia::DbError(ruvia::DbError::Code::kCancelled, ruvia::DbDriver::kPostgreSql, "cancelled");
        co_return;
    };
    auto complete = [](Guard operation, std::pmr::string input) -> ruvia::Task<void> {
        operation.start();
        (void)input;
        operation.finishActive();
        co_return;
    };

    State state(Lease("lease"));
    {
        auto cold = cancel(Guard(state), std::pmr::string(256, 'c', &memory));
        auto pending = complete(Guard(state), std::pmr::string(256, 'p', &memory));
        RUVIA_CHECK_EQ(memory.liveAllocations(), 2U * owned_string_allocations());
        asio::io_context io(1);
        auto future = asio::co_spawn(io, ruvia::asAwaitable(std::move(cold)), asio::use_future);
        io.run();
        bool cancellationObserved = false;
        try {
            future.get();
        } catch (const ruvia::DbError& error) {
            cancellationObserved = error.code() == ruvia::DbError::Code::kCancelled;
        }
        RUVIA_CHECK(cancellationObserved);
        RUVIA_CHECK(!state.active());
        RUVIA_CHECK_EQ(memory.liveAllocations(), owned_string_allocations());

        io.restart();
        auto pendingFuture = asio::co_spawn(io, ruvia::asAwaitable(std::move(pending)), asio::use_future);
        io.run();
        bool pendingRejected = false;
        try {
            pendingFuture.get();
        } catch (const std::logic_error& error) {
            pendingRejected = std::string_view(error.what()) == "database resource is not active";
        }
        RUVIA_CHECK(pendingRejected);
        RUVIA_CHECK_EQ(memory.liveAllocations(), 0U);
    }
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(database_operation_guard_releases_before_scoped_join_expires_owner) {
    asio::io_context io;
    ruvia::detail::ScopedOperationScope scope;
    ruvia::test::CountingMemoryResource memory;
    GuardedLeaseState state(GuardedLease("lease"));
    bool ownerExpired = false;
    GuardedLeaseCapability capability(scope, state, ownerExpired);
    GuardedLeaseGate gate;

    auto operation = ruvia::detail::makeScopedOperation(scope,
        failGuardedLeaseAfterGate(GuardedLeaseGuard(state), gate, std::pmr::string(256, 'j', &memory)));
    auto overlap = ruvia::detail::makeScopedOperation(scope,
        completeGuardedLease(GuardedLeaseGuard(state), std::pmr::string(256, 'o', &memory)));
    auto runner = asio::co_spawn(io,
        ruvia::asAwaitable(awaitScopedOperation(operation)), asio::use_future);
    io.poll();
    RUVIA_CHECK(gate.continuation_ != nullptr);
    RUVIA_CHECK_EQ(memory.liveAllocations(), 2U * owned_string_allocations());

    auto overlapRunner = asio::co_spawn(io,
        ruvia::asAwaitable(awaitScopedOperation(overlap)), asio::use_future);
    io.restart();
    io.poll();
    bool overlapRejected = false;
    try {
        overlapRunner.get();
    } catch (const std::logic_error& error) {
        overlapRejected = std::string_view(error.what()) == "database operation is already in progress";
    }
    RUVIA_CHECK(overlapRejected);
    RUVIA_CHECK_EQ(memory.liveAllocations(), owned_string_allocations());

    auto joiner = asio::co_spawn(io,
        ruvia::asAwaitable(joinScopedOperations(scope)), asio::use_future);
    io.restart();
    io.poll();
    RUVIA_CHECK(!scope.active());
    RUVIA_CHECK(!ownerExpired);

    gate.resume();
    io.restart();
    io.run();
    bool operationFailed = false;
    try {
        runner.get();
    } catch (const std::runtime_error& error) {
        operationFailed = std::string_view(error.what()) == "scoped database operation failed";
    }
    joiner.get();

    RUVIA_CHECK(operationFailed);
    RUVIA_CHECK(ownerExpired);
    RUVIA_CHECK(!state.active());
    RUVIA_CHECK_EQ(memory.liveAllocations(), 0U);
    RUVIA_CHECK_EQ(memory.allocationCount(), memory.deallocationCount());
}

RUVIA_TEST(database_operation_guard_survives_moving_stable_owner_while_running) {
    struct Payload final {
        int value;
    };
    using State = ruvia::detail::DbOperationState<Payload>;
    using Guard = ruvia::detail::DbOperationGuard<Payload>;

    struct ResumeGate final {
        [[nodiscard]] bool await_ready() const noexcept {
            return false;
        }

        void await_suspend(std::coroutine_handle<> continuation) noexcept {
            continuation_ = continuation;
        }

        void await_resume() const noexcept {}

        void resume() noexcept {
            auto continuation = std::exchange(continuation_, {});
            continuation.resume();
        }

        std::coroutine_handle<> continuation_{};
    };

    struct Owner final {
        Owner()
            : state(std::make_unique<State>(Payload{7})) {}

        Owner(const Owner&) = delete;
        Owner& operator=(const Owner&) = delete;
        Owner(Owner&&) noexcept = default;

        std::unique_ptr<State> state;
    };

    auto operate = [](Guard operation, ResumeGate& gate, int& observedValue) -> ruvia::Task<void> {
        operation.start();
        co_await gate;
        observedValue = operation.lease().value;
        operation.finishActive();
    };

    Owner source;
    ResumeGate gate;
    int observedValue = 0;
    Guard reservation(*source.state);
    auto task = operate(std::move(reservation), gate, observedValue);

    asio::io_context io(1);
    auto future =
        asio::co_spawn(io, ruvia::asAwaitable(std::move(task)), asio::use_future);
    io.poll();
    RUVIA_CHECK(gate.continuation_ != nullptr);

    Owner moved(std::move(source));
    RUVIA_CHECK(source.state == nullptr);
    io.restart();
    gate.resume();
    io.run();
    future.get();

    RUVIA_CHECK_EQ(observedValue, 7);
    RUVIA_CHECK(moved.state->active());
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
    DbRegistryTestRuntime runtime;
#ifdef RUVIA_ENABLE_MARIADB
    const auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb};
#else
    const auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql};
#endif
    const std::array<ruvia::detail::DbDefinition, 2> definitions{{
        dbDefinition("analytics", config),
        dbDefinition("default", config),
    }};
    ruvia::detail::DbRegistry registry(
        runtime.ioContext, runtime.worker, std::pmr::get_default_resource(), definitions);
    ruvia::detail::ScopedOperationScope operationScope;

    bool defaultResolved = true;
    bool aliasResolved = true;
    try {
        (void)registry.get(operationScope);
    } catch (...) {
        defaultResolved = false;
    }
    try {
        (void)registry.get("analytics", operationScope);
    } catch (...) {
        aliasResolved = false;
    }
    RUVIA_CHECK(defaultResolved);
    RUVIA_CHECK(aliasResolved);
}

RUVIA_TEST(db_registry_reports_typed_not_configured_error) {
    DbRegistryTestRuntime runtime;
    ruvia::detail::DbRegistry registry(runtime.ioContext, runtime.worker,
        std::pmr::get_default_resource(), std::span<const ruvia::detail::DbDefinition>());
    ruvia::detail::ScopedOperationScope operationScope;

    bool defaultTyped = false;
    bool aliasTyped = false;
    try {
        (void)registry.get(operationScope);
    } catch (const ruvia::DbError& error) {
        defaultTyped =
            error.code() == ruvia::DbError::Code::kNotConfigured && !error.driver().has_value();
    }
    try {
        (void)registry.get("missing", operationScope);
    } catch (const ruvia::DbError& error) {
        aliasTyped =
            error.code() == ruvia::DbError::Code::kNotConfigured && !error.driver().has_value();
    }
    RUVIA_CHECK(defaultTyped);
    RUVIA_CHECK(aliasTyped);
}

RUVIA_TEST(db_registry_owns_nested_pmr_configuration) {
    TrackingResource sourceResource;
    std::pmr::unsynchronized_pool_resource targetResource;
    DbRegistryTestRuntime runtime;
    std::optional<ruvia::detail::DbDefinition> definition;
    auto config = testDbConfig();
    config.host = std::string(80, 'h');
    config.tls.mode = ruvia::client_tls_mode::disabled;
    config.username = std::string(80, 'u');
    config.password = std::string(80, 'p');
    config.database = std::string(80, 'd');
    definition.emplace(dbDefinition("default", config, &sourceResource));

    std::optional<ruvia::detail::DbRegistry> registry;
    registry.emplace(runtime.ioContext, runtime.worker, &targetResource,
        std::span<const ruvia::detail::DbDefinition>(&*definition, 1));
    definition.reset();
    sourceResource.release();
    registry.reset();

    RUVIA_CHECK(!sourceResource.deallocatedAfterRelease());
}

RUVIA_TEST(db_handle_copy_rejects_after_parent_scope_closes) {
    DbRegistryTestRuntime runtime;
#ifdef RUVIA_ENABLE_MARIADB
    const auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kMariaDb};
#else
    const auto config = ruvia::DbConfig{.driver = ruvia::DbDriver::kPostgreSql};
#endif
    const std::array definitions{dbDefinition("default", config)};
    ruvia::detail::DbRegistry registry(
        runtime.ioContext, runtime.worker, std::pmr::get_default_resource(), definitions);
    ruvia::detail::ScopedOperationScope operationScope;
    auto handle = registry.get(operationScope);
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
        config.tls.mode = ruvia::client_tls_mode::disabled;
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
}

RUVIA_TEST(db_migrator_validates_complete_configuration_before_allocating) {
    {
        auto config = testDbConfig();
        config.host = std::string(80, 'h');
        config.tls.mode = ruvia::client_tls_mode::disabled;
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
        config.tls.mode = ruvia::client_tls_mode::disabled;
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
    config.tls.mode = ruvia::client_tls_mode::disabled;
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

RUVIA_TEST(db_sql_literal_cold_operations_release_owned_parameters) {
    DbRegistryTestRuntime runtime;
    ruvia::test::CountingMemoryResource memory;
    ruvia::detail::DbRegistry registry(runtime.ioContext, runtime.worker, &memory, testDbConfig());
    ruvia::detail::ScopedOperationScope scope;
    auto handle = registry.get(scope);
    const auto baseline = memory.liveAllocations();
    for (int i = 0; i < 16; ++i) {
        {
#ifdef RUVIA_ENABLE_MARIADB
            auto query = handle.query<"SELECT ?">(std::string(200, 'q'));
            auto execute = handle.execute<"UPDATE t SET name = ?">(std::string(200, 'e'));
            auto stream = handle.queryStream<"SELECT ?">(std::string(200, 's'));
            auto noParams = handle.query<"SELECT 1">();
#else
            auto query = handle.query<"SELECT $1", ruvia::DbDriver::kPostgreSql>(std::string(200, 'q'));
            auto execute = handle.execute<"UPDATE t SET name = $1", ruvia::DbDriver::kPostgreSql>(std::string(200, 'e'));
            auto stream = handle.queryStream<"SELECT $1", ruvia::DbDriver::kPostgreSql>(std::string(200, 's'));
            auto noParams = handle.query<"SELECT 1", ruvia::DbDriver::kPostgreSql>();
#endif
            RUVIA_CHECK(memory.liveAllocations() > baseline);
        }
        RUVIA_CHECK_EQ(memory.liveAllocations(), baseline);
    }
    const auto allocations = memory.allocationCount();
    const bool rejected = throwsOn([&] {
#ifdef RUVIA_ENABLE_MARIADB
        (void)handle.query<"SELECT $1", ruvia::DbDriver::kPostgreSql>(1);
#else
        (void)handle.query<"SELECT ?">(1);
#endif
    });
    RUVIA_CHECK(rejected);
    RUVIA_CHECK_EQ(memory.allocationCount(), allocations);
}

RUVIA_TEST(database_tls_configuration_enforces_backend_identity_constraints) {
#ifdef RUVIA_ENABLE_MARIADB
    ruvia::DbConfig maria{.driver = ruvia::DbDriver::kMariaDb};
    RUVIA_CHECK(maria.tls.mode == ruvia::client_tls_mode::verify_identity);
    RUVIA_CHECK(!ruvia::testing::throwsOn([&] { ruvia::detail::validateDbConfig(maria); }));
    maria.host = "database.example.test";
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { ruvia::detail::validateDbConfig(maria); }));
    maria.host = "127.0.0.1";
    maria.tls.server_name = "database.example.test";
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { ruvia::detail::validateDbConfig(maria); }));
    maria.tls = {.mode = ruvia::client_tls_mode::disabled};
    maria.host = "database.example.test";
    RUVIA_CHECK(!ruvia::testing::throwsOn([&] { ruvia::detail::validateDbConfig(maria); }));
#endif
#ifdef RUVIA_ENABLE_POSTGRESQL
    ruvia::DbConfig postgres{.driver = ruvia::DbDriver::kPostgreSql, .host = "database.example.test"};
    postgres.tls.server_name = "expected.example.test";
    RUVIA_CHECK(!ruvia::testing::throwsOn([&] { ruvia::detail::validateDbConfig(postgres); }));
    postgres.tls.ca_file = std::string("bad\0file", 8);
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { ruvia::detail::validateDbConfig(postgres); }));
#endif
}
