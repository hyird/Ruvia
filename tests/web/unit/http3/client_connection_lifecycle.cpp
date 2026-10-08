#include "http3_client_connection_fixture.h"

namespace {

struct Observation final {
    bool coldCancelled{};
    bool runningCancelled{};
    bool joined{};
    bool storageReleased{};
    bool invalidIdleTimeoutRejected{};
};

ruvia::Task<void> exercise(asio::io_context& io, const ruvia::WorkerHandle& worker,
    ruvia::EventLoopAttachment& attachment, Observation& observed,
    ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config;
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            try {
                Connection invalid(io, worker, tasks, tls, ruvia::HttpOriginView::https({.host = "localhost", .port = 49529}), 2s,
                    &memory, 4, 16 * 1024 * 1024, 0ms);
            } catch (const std::invalid_argument&) {
                observed.invalidIdleTimeoutRejected = true;
            }
            for (const auto invalid : {0ms, -1ms, std::chrono::milliseconds::max()}) {
                const auto baseline = memory.liveBytes;
                RUVIA_CHECK(ruvia::testing::throwsOn([&] {
                    Connection rejected(io, worker, tasks, tls, ruvia::HttpOriginView::https({.host = "localhost", .port = 49529}),
                        invalid, &memory);
                }));
                RUVIA_CHECK(ruvia::testing::throwsOn([&] {
                    Connection rejected(io, worker, tasks, tls, ruvia::HttpOriginView::https({.host = "localhost", .port = 49529}),
                        2s, &memory, 4, 16 * 1024 * 1024, invalid);
                }));
                RUVIA_CHECK_EQ(memory.liveBytes, baseline);
            }
            ruvia::detail::Http3ClientBodyBudget receiveBodyBudget(1024);
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "localhost", .port = 49529}), 2s,
                &memory, 4, 16 * 1024 * 1024, 30s, &receiveBodyBudget);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientResponseState responseState(worker, &memory);
            ruvia::detail::HttpClientRequestStorage boundRequest("GET", "/response-state", &memory);
            const auto bound = connection.submit(std::move(boundRequest), responseState);
            RUVIA_CHECK(bound.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(responseState.references == 2);
            RUVIA_CHECK(responseState.hasHttp3BodyBudget());
            RUVIA_CHECK_EQ(receiveBodyBudget.used(), std::size_t{0});
            RUVIA_CHECK(responseState.transport == ruvia::detail::HttpClientResponseTransport::kHttp3);
            RUVIA_CHECK(responseState.http3Connection == &connection);
            bool boundWaiterDone = false;
            auto boundWaiter = [&]() -> ruvia::Task<void> {
                co_await connection.wait(bound.id);
                boundWaiterDone = true;
            };
            tasks.spawn(boundWaiter());
            RUVIA_CHECK(!connection.releaseResponseRequest(bound.id));
            RUVIA_CHECK(co_await ruvia::sleepFor(worker, 1ms) == ruvia::TimerSleepResult::kElapsed);
            RUVIA_CHECK(!boundWaiterDone);
            const auto retainedFailure =
                std::make_exception_ptr(std::runtime_error("preserved response failure"));
            connection.cancel(bound.id);
            responseState.failure = retainedFailure;
            RUVIA_CHECK(connection.result(bound.id) && responseState.complete);
            RUVIA_CHECK(!connection.releaseResponseRequest(bound.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{1});
            RUVIA_CHECK(responseState.references == 2);
            RUVIA_CHECK(co_await ruvia::sleepFor(worker, 1ms) == ruvia::TimerSleepResult::kElapsed);
            RUVIA_CHECK(boundWaiterDone);
            RUVIA_CHECK(connection.releaseResponseRequest(bound.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
            RUVIA_CHECK(responseState.references == 1);
            RUVIA_CHECK(responseState.transport == ruvia::detail::HttpClientResponseTransport::kUnassigned);
            RUVIA_CHECK(responseState.http3Connection == nullptr && responseState.http3RequestId == 0);
            RUVIA_CHECK(!responseState.hasHttp3BodyBudget());
            RUVIA_CHECK(responseState.errorCode.has_value());
            RUVIA_CHECK_EQ(*responseState.errorCode,
                static_cast<std::uint8_t>(ruvia::HttpClientError::Code::kCancelled));
            RUVIA_CHECK(responseState.complete);
            RUVIA_CHECK(responseState.failure == retainedFailure);
            RUVIA_CHECK_EQ(receiveBodyBudget.used(), std::size_t{0});

            ruvia::detail::HttpClientResponseState localState(worker, &memory);
            Connection localBudgetConnection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https({.host = "localhost", .port = 49529}), 2s,
                &memory, 4, 16 * 1024 * 1024);
            const auto local = localBudgetConnection.submit(
                ruvia::detail::HttpClientRequestStorage("GET", "/local-budget", &memory),
                localState);
            RUVIA_CHECK(local.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(!localBudgetConnection.releaseResponseRequest(local.id));
            localState.pending.assign("retained");
            RUVIA_CHECK(localState.replaceProducerBodyBytes(localState.pending.size()));
            localBudgetConnection.cancel(local.id);
            RUVIA_CHECK(localBudgetConnection.result(local.id) && localState.complete);
            RUVIA_CHECK(!localBudgetConnection.releaseResponseRequest(local.id));
            RUVIA_CHECK_EQ(localState.pending, std::string_view("retained"));
            localState.discardResponseBody();
            RUVIA_CHECK(localBudgetConnection.releaseResponseRequest(local.id));
            RUVIA_CHECK_EQ(localState.errorCode.value(),
                static_cast<std::uint8_t>(ruvia::HttpClientError::Code::kCancelled));
            RUVIA_CHECK_EQ(localState.references, std::size_t{1});
            struct OriginCase {
                std::string_view host;
                std::uint16_t port;
                std::string_view authority;
            };
            constexpr std::array origins{
                OriginCase{"localhost", 443, "localhost"},
                OriginCase{"localhost", 49529, "localhost:49529"},
                OriginCase{"[::1]", 443, "[::1]"},
                OriginCase{"[::1]", 8443, "[::1]:8443"}};
            for (const auto& item : origins) {
                std::string host(item.host);
                Connection selected(io, worker, tasks, tls,
                    ruvia::HttpOriginView::https({.host = host, .port = item.port}), 2s,
                    &memory, 4, 16 * 1024 * 1024, 30s, &receiveBodyBudget);
                host.assign("modified-after-construction.invalid");
                ruvia::detail::HttpClientRequestStorage matching("GET", "/origin", &memory);
                matching.appendHeader("Host", item.authority);
                const auto admitted = selected.submit(std::move(matching));
                RUVIA_CHECK(admitted.outcome == Connection::Outcome::kPending);
                selected.cancel(admitted.id);
                RUVIA_CHECK(selected.release(admitted.id));
                ruvia::detail::HttpClientRequestStorage conflicting("GET", "/origin", &memory);
                conflicting.appendHeader("Host", "different.invalid");
                RUVIA_CHECK(selected.submit(std::move(conflicting)).outcome == Connection::Outcome::kInvalidRequest);
            }
            RUVIA_CHECK(ruvia::testing::throwsOn([&] {
                Connection plain(io, worker, tasks, tls,
                    ruvia::HttpOriginView::http({.host = "localhost"}), 2s, &memory);
            }));
            const auto beforeExpired = memory.liveBytes;
            const auto expired = connection.submit(Connection::RejectedRequest{
                .request = ruvia::detail::HttpClientRequestStorage("POST", "/expired-handoff", &memory),
                .deadline = Connection::TimePoint::min()});
            RUVIA_CHECK(expired.outcome == Connection::Outcome::kDeadline);
            RUVIA_CHECK_EQ(connection.retainedRequests(), 0U);
            RUVIA_CHECK_EQ(memory.liveBytes, beforeExpired);
            ruvia::detail::HttpClientRequestStorage cold("GET", "/cold", &memory);
            const auto first = connection.submit(std::move(cold));
            RUVIA_CHECK(first.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(!connection.takeRejectedRequest(first.id));
            connection.cancel(first.id);
            RUVIA_CHECK(!connection.takeRejectedRequest(first.id));
            RUVIA_CHECK(!connection.releaseResponseRequest(first.id));
            RUVIA_CHECK(connection.result(first.id) &&
                        !connection.result(first.id)->responseBodyPlan);
            observed.coldCancelled = connection.result(first.id) &&
                                     connection.result(first.id)->outcome ==
                                         Connection::Outcome::kCancelled;
            RUVIA_CHECK(connection.release(first.id));
            RUVIA_CHECK(!connection.takeRejectedRequest(first.id));

            ruvia::detail::HttpClientRequestStorage active("GET", "/active", &memory);
            ruvia::detail::HttpClientRequestStorage queued("GET", "/queued", &memory);
            const auto second = connection.submit(std::move(active));
            const auto third = connection.submit(std::move(queued));
            RUVIA_CHECK(second.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(third.outcome == Connection::Outcome::kPending);
            connection.start();  // The sole Task is now waiting on DNS or UDP.
            connection.requestStop();
            co_await connection.wait(second.id);
            co_await connection.wait(third.id);
            observed.runningCancelled = connection.result(second.id) &&
                                        connection.result(third.id) &&
                                        connection.result(second.id)->outcome == Connection::Outcome::kCancelled &&
                                        connection.result(third.id)->outcome == Connection::Outcome::kCancelled;
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            observed.joined = !connection.running();
            RUVIA_CHECK(connection.release(second.id));
            RUVIA_CHECK(connection.release(third.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), 0U);
        }
        observed.storageReleased = memory.allocations == memory.returns && memory.liveBytes == 0;
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exerciseSocketStop(asio::io_context& io, const ruvia::WorkerHandle& worker,
    ruvia::EventLoopAttachment& attachment, std::uint16_t port, Observation& observed,
    ruvia::testing::TestContext& ruvia_ctx, std::chrono::milliseconds timeout) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config;
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls, ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = port}), timeout,
                &memory, 32, 16 * 1024 * 1024, timeout);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientRequestStorage request("GET", "/blackhole", &memory);
            const auto submitted = connection.submit(std::move(request));
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();
            RUVIA_CHECK(co_await ruvia::sleepFor(worker, 40ms) ==
                        ruvia::TimerSleepResult::kElapsed);
            observed.coldCancelled = connection.running() && !connection.result(submitted.id);
            connection.requestStop();
            co_await connection.wait(submitted.id);
            observed.runningCancelled = connection.result(submitted.id) &&
                                        connection.result(submitted.id)->outcome == Connection::Outcome::kCancelled;
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            observed.joined = !connection.running();
            RUVIA_CHECK(connection.release(submitted.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), 0U);
        }
        observed.storageReleased = memory.allocations == memory.returns && memory.liveBytes == 0;
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exerciseTerminalTimeout(asio::io_context& io, const ruvia::WorkerHandle& worker,
    ruvia::EventLoopAttachment& attachment, std::uint16_t port, Observation& observed,
    ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config;
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls, ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = port}), 80ms, &memory);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientRequestStorage request("GET", "/timeout", &memory);
            const auto submitted = connection.submit(std::move(request));
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();
            co_await connection.wait(submitted.id);
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            observed.runningCancelled = connection.result(submitted.id) &&
                                        connection.result(submitted.id)->outcome == Connection::Outcome::kDeadline;
            ruvia::detail::HttpClientRequestStorage later("GET", "/later", &memory);
            observed.joined = !connection.running() &&
                              connection.submit(std::move(later)).outcome == Connection::Outcome::kConnectionDraining;
            RUVIA_CHECK(connection.release(submitted.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), 0U);
            RUVIA_CHECK_EQ(connection.retainedResultBodyBytes(), 0U);
        }
        observed.storageReleased = memory.allocations == memory.returns && memory.liveBytes == 0;
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exercisePerRequestDeadline(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    std::uint16_t port, Observation& observed, ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config;
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls, ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = port}), 2s, &memory);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientRequestStorage first("GET", "/first", &memory);
            ruvia::detail::HttpClientRequestStorage second("GET", "/short", &memory);
            const auto ongoing = connection.submit(std::move(first));
            const auto started = std::chrono::steady_clock::now();
            const auto shortWait = connection.submit(Connection::RejectedRequest{
                .request = std::move(second), .deadline = started + 35ms});
            RUVIA_CHECK(ongoing.outcome == Connection::Outcome::kPending);
            RUVIA_CHECK(shortWait.outcome == Connection::Outcome::kPending);
            connection.start();
            co_await connection.wait(shortWait.id);
            observed.coldCancelled = connection.result(shortWait.id) &&
                                     connection.result(shortWait.id)->outcome == Connection::Outcome::kDeadline &&
                                     !connection.result(ongoing.id) &&
                                     std::chrono::steady_clock::now() - started < 500ms;
            connection.requestStop();
            co_await connection.wait(ongoing.id);
            observed.runningCancelled = connection.result(ongoing.id) &&
                                        connection.result(ongoing.id)->outcome == Connection::Outcome::kCancelled;
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            observed.joined = !connection.running();
            RUVIA_CHECK(connection.release(ongoing.id));
            RUVIA_CHECK(connection.release(shortWait.id));
        }
        observed.storageReleased = memory.allocations == memory.returns && memory.liveBytes == 0;
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exerciseWriteInactivityTimeout(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    local_quic_response_peer& peer, ruvia::testing::TestContext& ruvia_ctx) {
    try {
        CountingResource memory;
        {
            ruvia::detail::ClientTransportConfigView config{
                .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
            ruvia::detail::http3_quic_client_tls_context tls(config);
            ruvia::TaskScope tasks(worker, {.resource = &memory});
            Connection connection(io, worker, tasks, tls,
                ruvia::HttpOriginView::https(
                    {.host = "127.0.0.1", .port = peer.port()}),
                5s, &memory, 4, 16 * 1024 * 1024, 30s, nullptr, 120ms);
            ConnectionWatchdog watchdog(io, connection);
            ruvia::detail::HttpClientRequestStorage request(
                "POST", "/write-timeout", &memory);
            const std::string body(4 * 1024 * 1024, 'x');
            request.setBody(body);
            const auto started = std::chrono::steady_clock::now();
            const auto submitted = connection.submit(std::move(request));
            RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
            connection.start();
            co_await connection.wait(submitted.id);
            const auto elapsed = std::chrono::steady_clock::now() - started;
            const auto result = connection.result(submitted.id);
            RUVIA_CHECK(result != nullptr);
            if (result != nullptr) {
                RUVIA_CHECK(result->outcome == Connection::Outcome::kDeadline);
            }
            RUVIA_CHECK(elapsed >= 100ms && elapsed < 3s);
            RUVIA_CHECK(peer.synchronize());
            RUVIA_CHECK(peer.handshake_observed());
            connection.requestStop();
            co_await tasks.join();
            watchdog.disarm();
            RUVIA_CHECK(!watchdog.expired());
            RUVIA_CHECK(connection.release(submitted.id));
            RUVIA_CHECK_EQ(connection.retainedRequests(), std::size_t{0});
        }
        RUVIA_CHECK_EQ(memory.allocations, memory.returns);
        RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

ruvia::Task<void> exercise_migration_retirement(asio::io_context& io,
    const ruvia::WorkerHandle& worker, ruvia::EventLoopAttachment& attachment,
    local_quic_response_peer& peer, ruvia::testing::TestContext& ruvia_ctx) {
    CountingResource memory;
    std::optional<std::uint64_t> migration_id;
    {
        ruvia::detail::ClientTransportConfigView tls_config{
            .tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kSkipVerification};
        ruvia::detail::http3_quic_client_tls_context tls(tls_config);
        ruvia::TaskScope tasks(worker, {.resource = &memory});
        Connection connection(io, worker, tasks, tls,
            ruvia::HttpOriginView::https({.host = "127.0.0.1", .port = peer.port()}), 10s,
            &memory, 4, 16 * 1024 * 1024, 2s);
        ruvia::detail::HttpClientRequestStorage request("GET", "/public-pool", &memory);
        const auto submitted = connection.submit(
            std::move(request), std::chrono::steady_clock::now() + 10s);
        RUVIA_CHECK(submitted.outcome == Connection::Outcome::kPending);
        connection.start();
        peer.allow_final_part();
        co_await connection.wait(submitted.id);
        const auto completed = connection.result(submitted.id);
        RUVIA_CHECK(completed != nullptr);
        if (completed != nullptr) {
            RUVIA_CHECK(completed->outcome == Connection::Outcome::kComplete);
        }

        asio::ip::udp::socket reservation(
            io, {asio::ip::address_v4::loopback(), 0});
        const auto local_endpoint = reservation.local_endpoint();
        reservation.close();
        const auto migration = connection.start_path_migration(local_endpoint);
        RUVIA_CHECK(migration.status == ruvia::quic_migration_status::started ||
                    migration.status == ruvia::quic_migration_status::validated);
        if (migration.status == ruvia::quic_migration_status::started ||
            migration.status == ruvia::quic_migration_status::validated) {
            migration_id = migration.id;
            const bool validated = co_await wait_for_peer(worker, peer, [&] {
                const auto status = connection.path_migration(*migration_id);
                return status && status->status != ruvia::quic_migration_status::started; }, 8s);
            RUVIA_CHECK(validated);
            const auto status = connection.path_migration(*migration_id);
            RUVIA_CHECK(status.has_value());
            if (status) {
                RUVIA_CHECK(status->status == ruvia::quic_migration_status::validated);
            }
        }

        const bool retired = co_await wait_for_peer(
            worker, peer, [&] { return !connection.running(); }, 5s);
        RUVIA_CHECK(retired);
        if (migration_id) {
            const auto result = connection.path_migration(*migration_id);
            RUVIA_CHECK(result.has_value());
            if (result) {
                RUVIA_CHECK(result->status == ruvia::quic_migration_status::validated);
            }
        }
        connection.requestStop();
        co_await tasks.join();
        if (submitted.outcome == Connection::Outcome::kPending) {
            RUVIA_CHECK(connection.release(submitted.id));
        }
    }
    RUVIA_CHECK_EQ(memory.allocations, memory.returns);
    RUVIA_CHECK_EQ(memory.liveBytes, std::size_t{0});
    attachment.stop();
}

}  // namespace

RUVIA_TEST(http3ClientConnectionRetainsValidatedMigrationAfterIdleSessionRetirement) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(
        exercise_migration_retirement(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientPoolRetainsValidatedMigrationAfterSessionRetirement) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(
        exercisePublicHttp3ClientPool(io, worker, attachment, peer, ruvia_ctx, false, {}, true));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientConnectionWriteInactivityTimeoutStopsAFlowControlledRequest) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    TestIdentityFiles identity;
    local_quic_response_peer peer(identity, false, false);
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    auto root = attachment.loop().start(
        exerciseWriteInactivityTimeout(io, worker, attachment, peer, ruvia_ctx));
    attachment.run();
    root.get();
#endif
}

RUVIA_TEST(http3ClientConnectionColdCancelAndStartedDriverStopJoinAreWorkerOwned) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    Observation observed;
    auto root = attachment.loop().start(exercise(io, worker, attachment, observed, ruvia_ctx));
    attachment.run();
    root.get();
    RUVIA_CHECK(observed.coldCancelled && observed.runningCancelled && observed.joined);
    RUVIA_CHECK(observed.storageReleased);
    RUVIA_CHECK(observed.invalidIdleTimeoutRejected);
#endif
}

RUVIA_TEST(http3ClientConnectionRequestDeadlineDoesNotFailOtherPendingRequests) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    auto& io = ruvia::test::newTestIoContext();
    asio::ip::udp::socket blackhole(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    Observation observed;
    auto root = attachment.loop().start(exercisePerRequestDeadline(io, worker, attachment,
        blackhole.local_endpoint().port(), observed, ruvia_ctx));
    attachment.run();
    root.get();
    RUVIA_CHECK(observed.coldCancelled && observed.runningCancelled && observed.joined);
    RUVIA_CHECK(observed.storageReleased);
#endif
}

RUVIA_TEST(http3ClientConnectionTerminalHandshakeTimeoutRejectsNewRequests) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    auto& io = ruvia::test::newTestIoContext();
    asio::ip::udp::socket blackhole(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    Observation observed;
    auto root = attachment.loop().start(exerciseTerminalTimeout(io, worker, attachment,
        blackhole.local_endpoint().port(), observed, ruvia_ctx));
    attachment.run();
    root.get();
    RUVIA_CHECK(observed.runningCancelled && observed.joined && observed.storageReleased);
#endif
}

RUVIA_TEST(http3ClientConnectionStopWakesPendingQuicSocketWaitAndJoinsDriver) {
#if OPENSSL_VERSION_NUMBER < 0x30600000L
    RUVIA_CHECK(true);
#else
    auto& io = ruvia::test::newTestIoContext();
    asio::ip::udp::socket blackhole(io,
        asio::ip::udp::endpoint(asio::ip::address_v4::loopback(), 0));
    auto attachment = ruvia::attachEventLoop(io);
    const auto worker = attachment.loop().handle();
    Observation observed;
    const auto largestTimeout = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::duration::max());
    auto root = attachment.loop().start(exerciseSocketStop(io, worker, attachment,
        blackhole.local_endpoint().port(), observed, ruvia_ctx, largestTimeout));
    attachment.run();
    root.get();
    RUVIA_CHECK(observed.coldCancelled && observed.runningCancelled && observed.joined);
    RUVIA_CHECK(observed.storageReleased);
#endif
}
