#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory_resource>
#include <optional>
#include <semaphore>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <asio/co_spawn.hpp>
#include <asio/io_context.hpp>
#include <asio/post.hpp>

#include "ruvia/core/AsioTask.h"
#include "ruvia/core/ConnectionScanner.h"
#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/Timer.h"
#include "ruvia/core/WorkerRuntimeContext.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/http/Http3FieldSection.h"
#include "ruvia/http/Http3Frames.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/CallbackRef.h"
#include "ruvia/web/detail/http3/Http3ServerConnection.h"
#include "ruvia/web/detail/http3/Http3ServerConnectionChannel.h"
#include "ruvia/web/detail/http3/Http3WorkerMailboxScheduler.h"
#include "ruvia/web/detail/http3/Http3WorkerServer.h"
#include "ruvia/web/detail/integration/WorkerCapabilities.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"
#include "ruvia/web/detail/server/HttpServerOptions.h"

#include "memory_resource_fixture.h"
#include "routing_fixture.h"
#include "test_harness.h"
#include "test_io_context.h"

namespace {

using Connection = ruvia::detail::Http3ServerConnection;
using ConnectionConfig = ruvia::detail::Http3ServerConnectionConfig;
using Scheduler = ruvia::detail::Http3WorkerMailboxScheduler;
using CapacitySignal = ruvia::detail::Http3WorkerMailboxCapacitySignal;
using Mailbox = ruvia::detail::Http3StreamMailbox;
using Control = ruvia::detail::Http3StreamControl;
using MessageId = ruvia::detail::Http3StreamMessageId;
using namespace std::chrono_literals;

constexpr std::uint64_t kEpoch = 109;
constexpr std::uint64_t kGeneration = 211;

struct RouteState final {
    std::string largeBody = std::string(48 * 1024, 'x');
};

ruvia::Task<ruvia::HttpResponse> schedulerHandler(void* raw, ruvia::Context& context) {
    auto& state = *static_cast<RouteState*>(raw);
    if (context.req().path() == "/large") {
        co_return context.text(std::string_view(state.largeBody));
    }
    co_return context.text(context.req().path());
}

struct Routes final {
    RouteState state;
    ruvia::detail::Router router;
    ruvia::detail::RouterImpl& implementation{ruvia::detail::RouterImpl::from(router)};

    Routes() {
        add(ruvia::HttpKnownMethod::kGet, "/small");
        add(ruvia::HttpKnownMethod::kGet, "/large");
        add(ruvia::HttpKnownMethod::kGet, "/deadline");
        add(ruvia::HttpKnownMethod::kHead, "/head");
        implementation.finalize();
    }

    void add(ruvia::HttpKnownMethod method, std::string_view path) {
        implementation.registerRoute(method, routing_test::path(path),
            ruvia::detail::RouteHandler(&state, &schedulerHandler),
            ruvia::detail::RequestBodyMode::kBuffered,
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{},
            std::span<const ruvia::detail::ControllerMiddlewareDescriptor>{});
    }
};

struct Fixture final {
    Routes routes;
    ruvia::WorkerMemory worker;
    ruvia::StopSource stopSource;
    ruvia::StopToken stopToken;
    ruvia::detail::ContextServices services;
    ruvia::detail::HttpServerOptions options;

    Fixture(const ruvia::WorkerHandle& workerHandle,
        std::pmr::memory_resource& upstream)
        : worker(upstream),
          stopToken(stopSource.token()),
          services(workerHandle, stopToken) {}
};

struct CrossThreadWake final {
    std::atomic<unsigned> notifications{};

    static void notify(void* context) noexcept {
        static_cast<CrossThreadWake*>(context)->notifications.fetch_add(
            1, std::memory_order_release);
    }
};

using WorkerServer = ruvia::detail::Http3WorkerServer;
using ConnectionChannel = ruvia::detail::Http3ServerConnectionChannel;

struct WorkerServerWakeBridge final {
    std::atomic<WorkerServer*> server{};
    std::atomic<unsigned> notifications{};

    static void notify(void* context) noexcept {
        auto& bridge = *static_cast<WorkerServerWakeBridge*>(context);
        bridge.notifications.fetch_add(1, std::memory_order_release);
        if (auto* server = bridge.server.load(std::memory_order_acquire); server != nullptr) {
            (void)server->notification().notify();
        }
    }
};

struct WorkerNotificationWake final {
    explicit WorkerNotificationWake(ruvia::WorkerNotification& target) noexcept
        : notification(target) {}

    static void activation(void* context) noexcept {
        auto& wake = *static_cast<WorkerNotificationWake*>(context);
        ++wake.activationNotifications;
        if (wake.notification.notify() == ruvia::WorkerNotificationStatus::kClosed) {
            std::terminate();
        }
    }

    static void capacity(void* context) noexcept {
        auto& wake = *static_cast<WorkerNotificationWake*>(context);
        ++wake.capacityNotifications;
        if (wake.notification.notify() == ruvia::WorkerNotificationStatus::kClosed) {
            std::terminate();
        }
    }

    ruvia::WorkerNotification& notification;
    unsigned activationNotifications{};
    unsigned capacityNotifications{};
};

ruvia::Task<void> exerciseWorkerServerPreLaunchRollback(
    ruvia::WorkerRuntimeContext& runtime, Fixture& fixture, Mailbox& requestMailbox,
    ConnectionChannel& channel, CrossThreadWake& networkWake,
    ruvia::testing::TestContext& ruvia_ctx) {
    const auto worker = runtime.handle();
    ruvia::detail::WorkerCapabilities capabilities(runtime.ioContext(), worker,
        fixture.worker.resource(), {}, {});
    ruvia::ConnectionScanner scanner(worker, {});
    std::atomic<std::size_t> activeConnections{};
    std::atomic<std::size_t> refusedConnections{};
    WorkerServer server(runtime, worker, fixture.worker,
        fixture.routes.implementation.routeTable(), capabilities, scanner,
        runtime.ioContext().get_executor(), fixture.options, fixture.stopToken, 1, 1,
        activeConnections, refusedConnections);
    const std::array<ConnectionChannel*, 1> channels{&channel};

    RUVIA_CHECK(!server.install());
    server.abandonBeforeLaunch();
    RUVIA_CHECK(server.drained());

    WorkerServer stopped(runtime, worker, fixture.worker,
        fixture.routes.implementation.routeTable(), capabilities, scanner,
        runtime.ioContext().get_executor(), fixture.options, fixture.stopToken, 1, 1,
        activeConnections, refusedConnections);
    RUVIA_CHECK(stopped.stageInstall({.requestMailbox = &requestMailbox,
        .channels = channels,
        .networkWake = {.context = &networkWake, .notify = &CrossThreadWake::notify}}));
    stopped.requestStop();
    Scheduler postStopScheduler(worker, 1, fixture.worker.resource());
    const auto grantAfterStop = channel.reserveAndPublishGrant(
        postStopScheduler, 7, 1);
    RUVIA_CHECK(grantAfterStop.status == ConnectionChannel::Status::kWrongState);
    RUVIA_CHECK(postStopScheduler.snapshot().freeConnections == std::size_t{1});
    RUVIA_CHECK(!stopped.install());
    stopped.abandonBeforeLaunch();
    RUVIA_CHECK(stopped.drained());
    runtime.close();
    co_return;
}

ruvia::Task<void> runProductionWorkerServer(
    ruvia::WorkerRuntimeContext& runtime, Fixture& fixture, Mailbox& requestMailbox,
    ConnectionChannel& channel, CrossThreadWake& networkWake,
    WorkerServerWakeBridge& workerWake, std::atomic<bool>& installed,
    std::atomic<bool>& stopRequested, ruvia::testing::TestContext& ruvia_ctx) {
    const auto worker = runtime.handle();
    ruvia::detail::WorkerCapabilities capabilities(runtime.ioContext(), worker,
        fixture.worker.resource(), {}, {});
    ruvia::ConnectionScanner scanner(worker, {});
    std::atomic<std::size_t> activeConnections{};
    std::atomic<std::size_t> refusedConnections{};
    WorkerServer server(runtime, worker, fixture.worker,
        fixture.routes.implementation.routeTable(), capabilities, scanner,
        runtime.ioContext().get_executor(), fixture.options, fixture.stopToken, 1, 1,
        activeConnections, refusedConnections);
    workerWake.server.store(&server, std::memory_order_release);
    const std::array<ConnectionChannel*, 1> channels{&channel};
    RUVIA_CHECK(server.stageInstall({.requestMailbox = &requestMailbox,
        .channels = channels,
        .networkWake = {.context = &networkWake, .notify = &CrossThreadWake::notify}}));
    if (!server.install()) {
        server.abandonBeforeLaunch();
        throw std::runtime_error("production HTTP/3 worker server install failed");
    }

    ruvia::TaskScope tasks(worker, {.resource = fixture.worker.resource()});
    try {
        tasks.spawn(server.run());
    } catch (...) {
        server.abandonBeforeLaunch();
        throw;
    }
    installed.store(true, std::memory_order_release);
    while (!stopRequested.load(std::memory_order_acquire)) {
        if (co_await ruvia::sleepFor(worker, 1ms, fixture.stopToken) !=
            ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    server.requestStop();
    co_await tasks.join();
    workerWake.server.store(nullptr, std::memory_order_release);
    runtime.close();
    static_cast<void>(ruvia_ctx);
}

bool accepted(Mailbox::SendResult result) noexcept {
    return result == Mailbox::SendResult::kSent ||
           result == Mailbox::SendResult::kSentNotifyPeer;
}

bool accepted(Mailbox::ControlResult result) noexcept {
    return result == Mailbox::ControlResult::kSent ||
           result == Mailbox::ControlResult::kSentNotifyPeer;
}

std::string frame(std::uint64_t type, std::string_view payload) {
    std::array<char, ruvia::kHttp3FrameHeaderMaxBytes> header{};
    const auto size = ruvia::encodeHttp3FrameHeader(header, type, payload.size());
    if (!size) {
        throw std::runtime_error("HTTP/3 scheduler test frame encoding failed");
    }
    std::string wire(header.data(), *size);
    wire.append(payload);
    return wire;
}

std::string requestWire(Fixture& fixture, std::string_view method, std::string_view path) {
    const auto encoded = ruvia::encodeHttp3ClientRequestHead({.method = method,
                                                                 .scheme = "https",
                                                                 .authority = "example.test",
                                                                 .path = path},
        {}, fixture.worker.resource());
    if (!encoded) {
        throw std::runtime_error("HTTP/3 scheduler request encoding failed");
    }
    return frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(encoded->fieldSection.data(), encoded->fieldSection.size()));
}

Connection::EventResult feedRequest(Connection& owner, Mailbox& inbound,
    Fixture& fixture, MessageId id, std::string_view method, std::string_view path) {
    const auto wire = requestWire(fixture, method, path);
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wire.data()), wire.size());
    if (!accepted(inbound.trySend(id, bytes)) ||
        !accepted(inbound.trySendControl(
            {Control::Kind::kStreamFin, id, static_cast<std::uint64_t>(wire.size())}))) {
        throw std::runtime_error("HTTP/3 scheduler input mailbox unexpectedly full");
    }
    Mailbox::BorrowedBlock block;
    if (!inbound.tryReceive(block)) {
        throw std::runtime_error("HTTP/3 scheduler input block was not published");
    }
    const auto result = owner.acceptData(block);
    block.release();
    (void)inbound.drainReturns();
    Control fin;
    if (!inbound.tryReceiveControl(fin)) {
        throw std::runtime_error("HTTP/3 scheduler input FIN was not published");
    }
    const auto finResult = owner.acceptControl(fin);
    (void)inbound.finishDrain();
    if (result.status != Connection::EventStatus::kAccepted) {
        return result;
    }
    return finResult;
}

Connection::EventResult feedMalformedHeaders(Connection& owner, Mailbox& inbound,
    Fixture& fixture, MessageId id) {
    const std::array<ruvia::Http3FieldSectionFieldView, 4> fields{{{":method", "GET"}, {":scheme", "https"}, {"x-before-path", "bad"}, {":path", "/"}}};
    const auto section = ruvia::encodeHttp3FieldSection(fields, fixture.worker.resource());
    if (!section) {
        throw std::runtime_error("HTTP/3 malformed scheduler request encoding failed");
    }
    const auto wire = frame(static_cast<std::uint64_t>(ruvia::Http3FrameType::kHeaders),
        std::string_view(section->data(), section->size()));
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(wire.data()), wire.size());
    if (!accepted(inbound.trySend(id, bytes))) {
        throw std::runtime_error("HTTP/3 malformed scheduler request mailbox full");
    }
    Mailbox::BorrowedBlock block;
    if (!inbound.tryReceive(block)) {
        throw std::runtime_error("HTTP/3 malformed scheduler block was not published");
    }
    const auto result = owner.acceptData(block);
    block.release();
    (void)inbound.drainReturns();
    (void)inbound.finishDrain();
    return result;
}

ruvia::Task<void> waitForReady(std::span<Connection* const> connections,
    const ruvia::WorkerHandle& worker, const ruvia::StopToken& stopToken) {
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        bool ready = true;
        for (const auto* connection : connections) {
            ready = ready && connection != nullptr && connection->readyRequestCount() != 0;
        }
        if (ready) {
            co_return;
        }
        if (co_await ruvia::sleepFor(worker, 1ms, stopToken) !=
            ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    throw std::runtime_error("HTTP/3 scheduler requests did not become publishable");
}

ruvia::Task<void> waitForNoTasks(std::span<Connection* const> connections,
    const ruvia::WorkerHandle& worker, const ruvia::StopToken& stopToken) {
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        bool idle = true;
        for (const auto* connection : connections) {
            idle = idle && connection != nullptr && connection->activeTaskCount() == 0;
        }
        if (idle) {
            co_return;
        }
        if (co_await ruvia::sleepFor(worker, 1ms, stopToken) !=
            ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    throw std::runtime_error("HTTP/3 scheduler request tasks did not retire");
}

ruvia::Task<void> stopAndRetire(Scheduler& scheduler, Scheduler::ConnectionToken token,
    Connection& connection, ruvia::testing::TestContext& ruvia_ctx);

ruvia::Task<void> exerciseActivationWakeWindow(ruvia::WorkerRuntimeContext& runtime,
    Fixture& fixture, bool activationBeforeWait, ruvia::testing::TestContext& ruvia_ctx) {
    const auto worker = runtime.handle();
    ruvia::WorkerNotification notification(runtime);
    WorkerNotificationWake wake(notification);
    CapacitySignal capacity({.context = &wake, .notify = &WorkerNotificationWake::capacity});
    Scheduler scheduler(worker, 1, fixture.worker.resource(), &capacity,
        Scheduler::kDefaultControlBurstLimit,
        {.context = &wake, .notify = &WorkerNotificationWake::activation});
    Mailbox outbound(8, 8, 8, fixture.worker.resource(), capacity.notifier());
    Mailbox inbound(2, 2, 2, fixture.worker.resource());
    RUVIA_CHECK(scheduler.bindMailbox(outbound));
    const auto registration = scheduler.reserve(kEpoch, kGeneration);
    RUVIA_CHECK(registration.has_value());
    if (!registration) {
        throw std::runtime_error("HTTP/3 activation wake test could not reserve a slot");
    }
    Connection owner(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, registration->activation,
        ConnectionConfig{.epoch = kEpoch,
            .connectionGeneration = kGeneration,
            .maxTrackedStreams = 8});
    RUVIA_CHECK(scheduler.attach(registration->token, owner));

    bool requestFed = false;
    std::exception_ptr requestFailure;
    if (activationBeforeWait) {
        const auto request = feedRequest(owner, inbound, fixture,
            {kEpoch, kGeneration, 0}, "GET", "/deadline");
        RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
        std::array<Connection*, 1> owners{&owner};
        co_await waitForReady(owners, worker, fixture.stopToken);
        RUVIA_CHECK(wake.activationNotifications != 0);
        RUVIA_CHECK_EQ(capacity.generation(), std::uint64_t{0});
    } else {
        // Posting while this coroutine is running guarantees the WorkerNotification
        // wait is armed before the request can produce its local activation.
        asio::post(runtime.ioContext(), [&] {
            try {
                const auto request = feedRequest(owner, inbound, fixture,
                    {kEpoch, kGeneration, 0}, "GET", "/deadline");
                requestFed = request.status == Connection::EventStatus::kDispatched;
            } catch (...) {
                requestFailure = std::current_exception();
            }
        });
    }

    RUVIA_CHECK(co_await notification.wait() ==
                ruvia::WorkerNotificationWaitStatus::kNotified);
    if (requestFailure) {
        std::rethrow_exception(requestFailure);
    }
    if (!activationBeforeWait) {
        RUVIA_CHECK(requestFed);
        std::array<Connection*, 1> owners{&owner};
        co_await waitForReady(owners, worker, fixture.stopToken);
    }
    RUVIA_CHECK(wake.activationNotifications != 0);
    RUVIA_CHECK_EQ(capacity.generation(), std::uint64_t{0});
    RUVIA_CHECK(scheduler.takeLocalWakeObligation());

    std::string responseWire;
    const auto pumpAndDrain = [&] {
        for (std::size_t turn = 0; turn < 64; ++turn) {
            const auto step = scheduler.step();
            if (step.kind == Scheduler::StepKind::kIdle) {
                break;
            }
            if (step.kind == Scheduler::StepKind::kWrongWorker ||
                step.kind == Scheduler::StepKind::kTransportIntent) {
                std::terminate();
            }
        }
        Mailbox::BorrowedBlock block;
        while (outbound.tryReceive(block)) {
            const auto bytes = block.bytes();
            responseWire.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            block.release();
        }
        Control control;
        while (outbound.tryReceiveControl(control)) {
        }
        (void)outbound.finishDrain();
        (void)outbound.drainReturns();
    };
    pumpAndDrain();
    std::array<Connection*, 1> owners{&owner};
    co_await waitForNoTasks(owners, worker, fixture.stopToken);
    pumpAndDrain();
    RUVIA_CHECK(responseWire.find("/deadline") != std::string::npos);

    if (!activationBeforeWait) {
        const auto notifier = capacity.notifier();
        notifier.notify(notifier.context);
        RUVIA_CHECK_EQ(capacity.generation(), std::uint64_t{1});
        RUVIA_CHECK(scheduler.snapshot().capacityWakePending);
        RUVIA_CHECK(wake.capacityNotifications != 0);
        RUVIA_CHECK(co_await notification.wait() ==
                    ruvia::WorkerNotificationWaitStatus::kNotified);
        (void)scheduler.step();
        RUVIA_CHECK(!scheduler.snapshot().capacityWakePending);
    }

    co_await stopAndRetire(scheduler, registration->token, owner, ruvia_ctx);
    (void)scheduler.takeLocalWakeObligation();
    RUVIA_CHECK(outbound.stop());
    notification.close();
}

void drainMailbox(Mailbox& mailbox, unsigned& dataBlocks, unsigned& controls) {
    Mailbox::BorrowedBlock block;
    while (mailbox.tryReceive(block)) {
        ++dataBlocks;
        block.release();
    }
    Control control;
    while (mailbox.tryReceiveControl(control)) {
        ++controls;
    }
    (void)mailbox.finishDrain();
    (void)mailbox.drainReturns();
}

ruvia::Task<void> stopAndRetire(Scheduler& scheduler, Scheduler::ConnectionToken token,
    Connection& connection, ruvia::testing::TestContext& ruvia_ctx) {
    RUVIA_CHECK(connection.requestStop());
    RUVIA_CHECK(scheduler.beginRetirement(token));
    std::optional<Scheduler::StepResult> close;
    for (std::size_t turn = 0; turn < 1024; ++turn) {
        const auto step = scheduler.step();
        if (step.kind == Scheduler::StepKind::kIdle) {
            break;
        }
        if (step.kind != Scheduler::StepKind::kTransportIntent) {
            continue;
        }
        if (step.connection == token &&
            step.intent.token.kind == Connection::TransportIntentKind::kConnectionClose) {
            close = step;
            break;
        }
        if (!scheduler.acknowledgeIntent(step.connection, step.intent.token)) {
            throw std::runtime_error("scheduler intent handoff could not be settled");
        }
    }
    RUVIA_CHECK(close.has_value());
    if (!close) {
        throw std::runtime_error("HTTP/3 close intent was not offered during retirement");
    }
    co_await connection.join();
    RUVIA_CHECK(connection.takeOverTransportRetirement(
        {.epoch = token.epoch, .connectionGeneration = token.connectionGeneration}));
    RUVIA_CHECK(scheduler.acknowledgeIntent(token, close->intent.token));
    while (connection.pendingTransportIntentCount() != 0) {
        const auto pending = scheduler.step();
        if (pending.kind != Scheduler::StepKind::kTransportIntent ||
            !scheduler.acknowledgeIntent(token, pending.intent.token)) {
            throw std::runtime_error("retirement intent debt could not be settled");
        }
    }
    RUVIA_CHECK(scheduler.retire(token));
}

ruvia::Task<void> exercise_retirement_waits_for_intent_acknowledgement(
    Fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx) {
    Scheduler scheduler(worker, 1, fixture.worker.resource());
    Mailbox outbound(2, 2, 2, fixture.worker.resource());
    RUVIA_CHECK(scheduler.bindMailbox(outbound));
    const auto registration = scheduler.reserve(kEpoch, kGeneration);
    RUVIA_CHECK(registration.has_value());
    if (!registration) {
        throw std::runtime_error("scheduler retirement fixture slot unavailable");
    }
    Connection owner(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, registration->activation,
        ConnectionConfig{.epoch = kEpoch,
            .connectionGeneration = kGeneration,
            .maxTrackedStreams = 8});
    RUVIA_CHECK(scheduler.attach(registration->token, owner));
    RUVIA_CHECK(owner.requestStop());
    const auto close = scheduler.step();
    RUVIA_CHECK(close.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(close.intent.token.kind == Connection::TransportIntentKind::kConnectionClose);
    RUVIA_CHECK(scheduler.beginRetirement(registration->token));
    co_await owner.join();
    RUVIA_CHECK(owner.confirmTransportRetired({.epoch = registration->token.epoch,
        .connectionGeneration = registration->token.connectionGeneration}));
    RUVIA_CHECK(!scheduler.retire(registration->token));
    RUVIA_CHECK(scheduler.acknowledgeIntent(registration->token, close.intent.token));
    RUVIA_CHECK(scheduler.retire(registration->token));
    RUVIA_CHECK(outbound.stop());
    static_cast<void>(ruvia_ctx);
}

ruvia::Task<void> stopAfter(ruvia::EventLoopAttachment& attachment,
    ruvia::Task<void> operation) {
    try {
        co_await std::move(operation);
    } catch (...) {
        attachment.stop();
        throw;
    }
    attachment.stop();
}

void runWorkerTask(ruvia::EventLoopAttachment& attachment, ruvia::Task<void> operation) {
    auto root = attachment.loop().start(stopAfter(attachment, std::move(operation)));
    attachment.run();
}

ruvia::Task<void> exerciseCrossConnectionLaneRotation(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    constexpr std::size_t kHeadConnections = 6;
    constexpr std::size_t kConnections = kHeadConnections + 1;
    CrossThreadWake wake;
    CapacitySignal capacity({.context = &wake, .notify = &CrossThreadWake::notify});
    Scheduler scheduler(worker, kConnections, fixture.worker.resource(), &capacity);
    Mailbox outbound(32, 32, 16, fixture.worker.resource(), capacity.notifier());
    RUVIA_CHECK(scheduler.bindMailbox(outbound));
    Mailbox inbound(2, 2, 2, fixture.worker.resource());
    std::array<std::optional<Connection>, kConnections> owners;
    std::array<Scheduler::ConnectionToken, kConnections> tokens{};
    std::array<Connection*, kConnections> ownerRefs{};

    for (std::size_t index = 0; index < kConnections; ++index) {
        const auto epoch = kEpoch + index;
        const auto generation = kGeneration + index;
        const auto registration = scheduler.reserve(epoch, generation);
        RUVIA_CHECK(registration.has_value());
        if (!registration) {
            throw std::runtime_error("scheduler connection slots unexpectedly exhausted");
        }
        tokens[index] = registration->token;
        owners[index].emplace(fixture.routes.implementation.routeTable(), fixture.worker,
            fixture.services, fixture.options, outbound, registration->activation,
            ConnectionConfig{.epoch = epoch,
                .connectionGeneration = generation,
                .maxTrackedStreams = 8});
        RUVIA_CHECK(scheduler.attach(tokens[index], *owners[index]));
        ownerRefs[index] = &*owners[index];
        const auto request = index < kHeadConnections
                                 ? feedRequest(*owners[index], inbound, fixture,
                                       {epoch, generation, 0}, "HEAD", "/head")
                                 : feedRequest(*owners[index], inbound, fixture,
                                       {epoch, generation, 0}, "GET", "/large");
        RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
    }
    co_await waitForReady(ownerRefs, worker, fixture.stopToken);
    (void)scheduler.takeLocalWakeObligation();

    unsigned dataBlocks = 0;
    unsigned controls = 0;
    unsigned peerWakeObligations = 0;
    std::size_t consecutiveControlTurns = 0;
    std::size_t maxConsecutiveControlTurns = 0;
    bool dataServedWhileControlPending = false;
    bool largeServed = false;
    std::size_t turns = 0;
    for (; turns < 256; ++turns) {
        const auto step = scheduler.step();
        if (step.kind == Scheduler::StepKind::kIdle) {
            break;
        }
        if (step.kind == Scheduler::StepKind::kWrongWorker) {
            RUVIA_CHECK(false);
            break;
        }
        if (step.kind != Scheduler::StepKind::kPublication) {
            continue;
        }
        RUVIA_CHECK_EQ(step.notifyPeer, step.publication.publication.notifyPeer);
        if (step.notifyPeer) {
            ++peerWakeObligations;
        }
        if (step.connection == tokens.back() &&
            step.publication.publication.status ==
                Connection::Dispatch::PublishStatus::kBytesPublished) {
            largeServed = true;
        }
        if (step.publication.publication.status ==
            Connection::Dispatch::PublishStatus::kFinPublished) {
            ++consecutiveControlTurns;
            maxConsecutiveControlTurns =
                std::max(maxConsecutiveControlTurns, consecutiveControlTurns);
        } else {
            consecutiveControlTurns = 0;
            const auto lanes = scheduler.snapshot();
            dataServedWhileControlPending = dataServedWhileControlPending ||
                                            (step.publication.publication.status ==
                                                    Connection::Dispatch::PublishStatus::kBytesPublished &&
                                                lanes.runnable[0] != 0 && lanes.runnable[1] != 0);
        }
        drainMailbox(outbound, dataBlocks, controls);
    }
    RUVIA_CHECK(turns < 256);
    RUVIA_CHECK(largeServed);
    RUVIA_CHECK(maxConsecutiveControlTurns <= Scheduler::kDefaultControlBurstLimit);
    RUVIA_CHECK(dataServedWhileControlPending);
    RUVIA_CHECK(dataBlocks > kHeadConnections);
    RUVIA_CHECK(controls >= kHeadConnections);
    RUVIA_CHECK(peerWakeObligations != 0);
    co_await waitForNoTasks(ownerRefs, worker, fixture.stopToken);

    for (std::size_t index = 0; index < kConnections; ++index) {
        co_await stopAndRetire(scheduler, tokens[index], *owners[index], ruvia_ctx);
        owners[index].reset();
    }
    (void)scheduler.takeLocalWakeObligation();
    RUVIA_CHECK(outbound.stop());
}

ruvia::Task<void> waitForAtomic(const std::atomic<bool>& ready,
    const ruvia::WorkerHandle& worker, const ruvia::StopToken& stopToken) {
    for (std::size_t attempt = 0; attempt < 2000; ++attempt) {
        if (ready.load(std::memory_order_acquire)) {
            co_return;
        }
        if (co_await ruvia::sleepFor(worker, 1ms, stopToken) !=
            ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    throw std::runtime_error("cross-thread capacity consumer did not finish");
}

ruvia::Task<void> exerciseCapacityWakeFansOutAndPreservesReset(
    Fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx) {
    constexpr std::size_t kDataConnections = 2;
    constexpr std::size_t kConnections = kDataConnections + 1;
    CrossThreadWake wake;
    CapacitySignal capacity({.context = &wake, .notify = &CrossThreadWake::notify});
    Scheduler scheduler(worker, kConnections, fixture.worker.resource(), &capacity);
    Mailbox outbound(4, 1, 1, fixture.worker.resource(), capacity.notifier());
    RUVIA_CHECK(scheduler.bindMailbox(outbound));
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    std::array<std::optional<Connection>, kConnections> owners;
    std::array<Scheduler::ConnectionToken, kConnections> tokens{};
    std::array<Connection*, kConnections> ownerRefs{};

    const std::array<std::byte, 1> filler{std::byte{0x42}};
    const MessageId fillerId{kEpoch + 90, kGeneration + 90, 0};
    RUVIA_CHECK(outbound.trySend(fillerId, filler) == Mailbox::SendResult::kSentNotifyPeer);
    RUVIA_CHECK(outbound.trySendControl(
                    {Control::Kind::kWritable, fillerId, 0}) == Mailbox::ControlResult::kSent);

    for (std::size_t index = 0; index < kConnections; ++index) {
        const auto epoch = kEpoch + 10 + index;
        const auto generation = kGeneration + 10 + index;
        const auto registration = scheduler.reserve(epoch, generation);
        RUVIA_CHECK(registration.has_value());
        if (!registration) {
            throw std::runtime_error("scheduler capacity fixture exhausted its slots");
        }
        tokens[index] = registration->token;
        owners[index].emplace(fixture.routes.implementation.routeTable(), fixture.worker,
            fixture.services, fixture.options, outbound, registration->activation,
            ConnectionConfig{.epoch = epoch,
                .connectionGeneration = generation,
                .maxTrackedStreams = 8});
        RUVIA_CHECK(scheduler.attach(tokens[index], *owners[index]));
        ownerRefs[index] = &*owners[index];
        if (index < kDataConnections) {
            const auto request = feedRequest(*owners[index], inbound, fixture,
                {epoch, generation, 0}, "GET", "/small");
            RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
        } else {
            const auto malformed = feedMalformedHeaders(*owners[index], inbound, fixture,
                {epoch, generation, 0});
            RUVIA_CHECK(malformed.status == Connection::EventStatus::kProtocolError);
            RUVIA_CHECK(!malformed.connectionCloseRequired);
        }
    }
    std::array<Connection*, kDataConnections> dataRefs{ownerRefs[0], ownerRefs[1]};
    co_await waitForReady(dataRefs, worker, fixture.stopToken);
    RUVIA_CHECK(scheduler.step().kind == Scheduler::StepKind::kPublication);
    auto intentStep = scheduler.step();
    RUVIA_CHECK(intentStep.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(intentStep.intent.token.kind == Connection::TransportIntentKind::kStreamReset);
    RUVIA_CHECK(intentStep.intent.streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kMessageError);
    RUVIA_CHECK(scheduler.parkIntentForControlCapacity(
        intentStep.connection, intentStep.intent.token));
    RUVIA_CHECK(scheduler.step().kind == Scheduler::StepKind::kPublication);

    auto state = scheduler.snapshot();
    RUVIA_CHECK_EQ(state.blocked[0], std::size_t{2});
    RUVIA_CHECK_EQ(state.blocked[2], std::size_t{1});
    RUVIA_CHECK(scheduler.armCapacityWait().status ==
                Scheduler::CapacityArmStatus::kArmed);
    RUVIA_CHECK(scheduler.snapshot().capacityWaitArmed);

    std::atomic<bool> consumerDone{};
    std::thread controlConsumer([&] {
        Control control;
        if (!outbound.tryReceiveControl(control)) {
            std::terminate();
        }
        Mailbox::BorrowedBlock block;
        while (outbound.tryReceive(block)) {
            block.release();
        }
        while (outbound.tryReceiveControl(control)) {
        }
        if (outbound.finishDrain()) {
            std::terminate();
        }
        consumerDone.store(true, std::memory_order_release);
    });
    co_await waitForAtomic(consumerDone, worker, fixture.stopToken);
    controlConsumer.join();
    RUVIA_CHECK_EQ(wake.notifications.load(std::memory_order_acquire), 1U);
    RUVIA_CHECK(scheduler.snapshot().capacityWakePending);
    scheduler.notifyTransportCapacity();
    (void)outbound.drainReturns();
    RUVIA_CHECK(accepted(outbound.trySend(fillerId, filler)));

    // The mailbox wake recovers response waiters; transport capacity independently
    // recovers the reset while preserving each DATA waiter's finite recovery turn.
    const auto resetPlan = scheduler.step();
    RUVIA_CHECK(resetPlan.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(resetPlan.intent.streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kMessageError);
    const Control resetControl{.kind = Control::Kind::kStreamReset,
        .id = resetPlan.intent.token.id,
        .streamResetErrorCode = resetPlan.intent.streamResetErrorCode};
    const auto resetSent = outbound.trySendControl(resetControl);
    RUVIA_CHECK(accepted(resetSent));
    RUVIA_CHECK(scheduler.acknowledgeIntent(resetPlan.connection,
        resetPlan.intent.token));
    RUVIA_CHECK(!scheduler.acknowledgeIntent(resetPlan.connection,
        resetPlan.intent.token));
    Control receivedReset;
    RUVIA_CHECK(outbound.tryReceiveControl(receivedReset));
    RUVIA_CHECK(receivedReset.streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kMessageError);

    const auto firstRecovery = scheduler.step();
    RUVIA_CHECK(firstRecovery.kind == Scheduler::StepKind::kPublication);
    RUVIA_CHECK(firstRecovery.publication.publication.status ==
                Connection::Dispatch::PublishStatus::kBackpressured);
    const auto secondRecovery = scheduler.step();
    RUVIA_CHECK(secondRecovery.kind == Scheduler::StepKind::kPublication);
    RUVIA_CHECK(secondRecovery.publication.publication.status ==
                Connection::Dispatch::PublishStatus::kBackpressured);

    const auto dataWait = scheduler.armCapacityWait();
    RUVIA_CHECK(dataWait.status == Scheduler::CapacityArmStatus::kArmed);
    RUVIA_CHECK(dataWait.interest == Mailbox::CapacityInterest::kData);
    consumerDone.store(false, std::memory_order_relaxed);
    std::thread dataConsumer([&] {
        Mailbox::BorrowedBlock block;
        if (!outbound.tryReceive(block)) {
            std::terminate();
        }
        block.release();
        Control control;
        while (outbound.tryReceiveControl(control)) {
        }
        if (outbound.finishDrain()) {
            std::terminate();
        }
        consumerDone.store(true, std::memory_order_release);
    });
    co_await waitForAtomic(consumerDone, worker, fixture.stopToken);
    dataConsumer.join();
    RUVIA_CHECK_EQ(wake.notifications.load(std::memory_order_acquire), 2U);
    const auto dataRecoveryOne = scheduler.step();
    const auto dataRecoveryTwo = scheduler.step();
    RUVIA_CHECK(dataRecoveryOne.kind == Scheduler::StepKind::kPublication);
    RUVIA_CHECK(dataRecoveryTwo.kind == Scheduler::StepKind::kPublication);
    RUVIA_CHECK(dataRecoveryOne.connection != dataRecoveryTwo.connection);
    RUVIA_CHECK(dataRecoveryOne.publication.publication.status !=
                Connection::Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(dataRecoveryTwo.publication.publication.status ==
                Connection::Dispatch::PublishStatus::kBackpressured);

    for (std::size_t index = 0; index < kConnections; ++index) {
        co_await stopAndRetire(scheduler, tokens[index], *owners[index], ruvia_ctx);
        owners[index].reset();
    }
    RUVIA_CHECK(outbound.stop());
    (void)scheduler.step();
    RUVIA_CHECK(!scheduler.snapshot().capacityWakePending);
    RUVIA_CHECK(!scheduler.snapshot().capacityWaitArmed);
    static_cast<void>(ruvia_ctx);
}

ruvia::Task<void> exerciseControlOnlyResetCapacityWait(
    Fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx) {
    CrossThreadWake wake;
    CapacitySignal capacity({.context = &wake, .notify = &CrossThreadWake::notify});
    Scheduler scheduler(worker, 1, fixture.worker.resource(), &capacity);
    Mailbox outbound(2, 1, 1, fixture.worker.resource(), capacity.notifier());
    RUVIA_CHECK(scheduler.bindMailbox(outbound));
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    RUVIA_CHECK(!scheduler.bindMailbox(inbound));
    const auto registration = scheduler.reserve(kEpoch + 40, kGeneration + 40);
    RUVIA_CHECK(registration.has_value());
    if (!registration) {
        throw std::runtime_error("scheduler CONTROL-only fixture slot unavailable");
    }
    std::optional<Connection> owner;
    owner.emplace(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, registration->activation,
        ConnectionConfig{.epoch = kEpoch + 40,
            .connectionGeneration = kGeneration + 40,
            .maxTrackedStreams = 8});
    RUVIA_CHECK(scheduler.attach(registration->token, *owner));
    const MessageId fillerId{kEpoch + 41, kGeneration + 41, 0};
    RUVIA_CHECK(outbound.trySendControl(
                    {Control::Kind::kWritable, fillerId, 0}) ==
                Mailbox::ControlResult::kSentNotifyPeer);
    const auto malformed = feedMalformedHeaders(*owner, inbound, fixture,
        {kEpoch + 40, kGeneration + 40, 0});
    RUVIA_CHECK(malformed.status == Connection::EventStatus::kProtocolError);
    const auto reset = scheduler.step();
    RUVIA_CHECK(reset.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(reset.intent.streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kMessageError);
    RUVIA_CHECK(scheduler.parkIntentForControlCapacity(
        reset.connection, reset.intent.token));
    const auto noWait = scheduler.armCapacityWait();
    RUVIA_CHECK(noWait.status == Scheduler::CapacityArmStatus::kNoBlockedWork);
    RUVIA_CHECK(!scheduler.snapshot().capacityWaitArmed);
    // Reset intents wait on the network owner's transport-capacity notification,
    // not the unrelated response CONTROL lane.
    scheduler.notifyTransportCapacity();
    RUVIA_CHECK(scheduler.snapshot().capacityPassLanes != 0);
    const auto recovered = scheduler.step();
    RUVIA_CHECK(recovered.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(recovered.intent.token == reset.intent.token);
    RUVIA_CHECK(recovered.intent.streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kMessageError);
    Control filler;
    RUVIA_CHECK(outbound.tryReceiveControl(filler));
    RUVIA_CHECK(!outbound.finishDrain());
    const Control resetControl{.kind = Control::Kind::kStreamReset,
        .id = recovered.intent.token.id,
        .streamResetErrorCode = recovered.intent.streamResetErrorCode};
    RUVIA_CHECK(accepted(outbound.trySendControl(resetControl)));
    RUVIA_CHECK(scheduler.acknowledgeIntent(recovered.connection,
        recovered.intent.token));
    Control receivedReset;
    RUVIA_CHECK(outbound.tryReceiveControl(receivedReset));
    RUVIA_CHECK(receivedReset.streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kMessageError);
    RUVIA_CHECK(!outbound.finishDrain());

    RUVIA_CHECK(owner->requestStop());
    const auto close = scheduler.step();
    RUVIA_CHECK(close.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(close.intent.token.kind == Connection::TransportIntentKind::kConnectionClose);
    RUVIA_CHECK(scheduler.beginRetirement(registration->token));
    co_await owner->join();
    RUVIA_CHECK(owner->takeOverTransportRetirement(
        {.epoch = registration->token.epoch,
            .connectionGeneration = registration->token.connectionGeneration}));
    RUVIA_CHECK(scheduler.acknowledgeIntent(registration->token, close.intent.token));
    RUVIA_CHECK(scheduler.retire(registration->token));
    owner.reset();
    RUVIA_CHECK(outbound.stop());
}

ruvia::Task<void> exerciseLocalDeadlineWithoutCapacityNotifier(
    Fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx) {
    fixture.options.deadline = ruvia::DeadlineConfig{.handler = 60ms};
    Scheduler scheduler(worker, 1, fixture.worker.resource());
    Mailbox outbound(2, 1, 1, fixture.worker.resource());
    RUVIA_CHECK(scheduler.bindMailbox(outbound));
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    const auto registration = scheduler.reserve(kEpoch + 50, kGeneration + 50);
    RUVIA_CHECK(registration.has_value());
    if (!registration) {
        throw std::runtime_error("scheduler local deadline slot unavailable");
    }
    std::optional<Connection> owner;
    owner.emplace(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, registration->activation,
        ConnectionConfig{.epoch = kEpoch + 50,
            .connectionGeneration = kGeneration + 50,
            .maxTrackedStreams = 8});
    RUVIA_CHECK(scheduler.attach(registration->token, *owner));
    const std::array<std::byte, 1> filler{std::byte{0x23}};
    RUVIA_CHECK(accepted(outbound.trySend(
        {kEpoch + 80, kGeneration + 80, 0}, filler)));
    const auto request = feedRequest(*owner, inbound, fixture,
        {kEpoch + 50, kGeneration + 50, 0}, "GET", "/deadline");
    RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
    std::array<Connection*, 1> references{&*owner};
    co_await waitForReady(references, worker, fixture.stopToken);
    (void)scheduler.takeLocalWakeObligation();

    const auto backpressure = scheduler.step();
    RUVIA_CHECK(backpressure.kind == Scheduler::StepKind::kPublication);
    RUVIA_CHECK(backpressure.publication.publication.status ==
                Connection::Dispatch::PublishStatus::kBackpressured);
    RUVIA_CHECK(backpressure.publication.publication.blockReason ==
                Connection::Dispatch::PublishBlockReason::kData);
    RUVIA_CHECK(scheduler.armCapacityWait().status ==
                Scheduler::CapacityArmStatus::kUnavailable);

    bool localReady = false;
    for (std::size_t attempt = 0; attempt < 1000; ++attempt) {
        const auto state = scheduler.snapshot();
        if (state.runnable[2] != 0) {
            localReady = true;
            break;
        }
        if (co_await ruvia::sleepFor(worker, 1ms, fixture.stopToken) !=
            ruvia::TimerSleepResult::kElapsed) {
            break;
        }
    }
    RUVIA_CHECK(localReady);
    RUVIA_CHECK(scheduler.takeLocalWakeObligation());
    const auto cancelled = scheduler.step();
    RUVIA_CHECK(cancelled.kind == Scheduler::StepKind::kPublication);
    RUVIA_CHECK(cancelled.publication.publication.status ==
                Connection::Dispatch::PublishStatus::kCancelled);
    const auto reset = scheduler.step();
    RUVIA_CHECK(reset.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(reset.intent.token.kind == Connection::TransportIntentKind::kStreamReset);
    RUVIA_CHECK(reset.intent.streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kRequestCancelled);
    RUVIA_CHECK(owner->requestStop());
    RUVIA_CHECK(scheduler.step().kind == Scheduler::StepKind::kIdle);
    RUVIA_CHECK(scheduler.acknowledgeIntent(registration->token, reset.intent.token));
    const auto close = scheduler.step();
    RUVIA_CHECK(close.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(close.intent.token.kind == Connection::TransportIntentKind::kConnectionClose);
    RUVIA_CHECK(scheduler.beginRetirement(registration->token));
    co_await owner->join();
    RUVIA_CHECK(owner->takeOverTransportRetirement(
        {.epoch = registration->token.epoch,
            .connectionGeneration = registration->token.connectionGeneration}));
    RUVIA_CHECK(scheduler.acknowledgeIntent(registration->token, close.intent.token));
    RUVIA_CHECK(scheduler.retire(registration->token));
    owner.reset();
    Control unused;
    while (outbound.tryReceiveControl(unused)) {
    }
    Mailbox::BorrowedBlock block;
    while (outbound.tryReceive(block)) {
        block.release();
    }
    (void)outbound.drainReturns();
    (void)outbound.finishDrain();
    RUVIA_CHECK(outbound.stop());
}

ruvia::Task<void> exerciseStaleSlotActivationAfterJoin(
    Fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx) {
    Scheduler scheduler(worker, 1, fixture.worker.resource());
    Mailbox outbound(2, 2, 2, fixture.worker.resource());
    RUVIA_CHECK(scheduler.bindMailbox(outbound));
    const auto oldRegistration = scheduler.reserve(kEpoch + 60, kGeneration + 60);
    RUVIA_CHECK(oldRegistration.has_value());
    if (!oldRegistration) {
        throw std::runtime_error("scheduler ABA fixture slot unavailable");
    }
    std::optional<Connection> oldOwner;
    oldOwner.emplace(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, oldRegistration->activation,
        ConnectionConfig{.epoch = kEpoch + 60,
            .connectionGeneration = kGeneration + 60,
            .maxTrackedStreams = 8});
    RUVIA_CHECK(scheduler.attach(oldRegistration->token, *oldOwner));
    RUVIA_CHECK(!scheduler.reserve(kEpoch + 61, kGeneration + 61).has_value());
    RUVIA_CHECK(oldOwner->requestStop());
    const auto oldClose = scheduler.step();
    RUVIA_CHECK(oldClose.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(scheduler.beginRetirement(oldRegistration->token));
    co_await oldOwner->join();
    RUVIA_CHECK(oldOwner->takeOverTransportRetirement(
        {.epoch = oldRegistration->token.epoch,
            .connectionGeneration = oldRegistration->token.connectionGeneration}));
    RUVIA_CHECK(scheduler.acknowledgeIntent(oldRegistration->token, oldClose.intent.token));
    RUVIA_CHECK(scheduler.retire(oldRegistration->token));
    oldOwner.reset();

    const auto currentRegistration = scheduler.reserve(kEpoch + 62, kGeneration + 62);
    RUVIA_CHECK(currentRegistration.has_value());
    if (!currentRegistration) {
        throw std::runtime_error("scheduler did not return a retired slot");
    }
    RUVIA_CHECK_EQ(currentRegistration->token.slot, oldRegistration->token.slot);
    RUVIA_CHECK(currentRegistration->token.slotGeneration !=
                oldRegistration->token.slotGeneration);
    std::optional<Connection> currentOwner;
    currentOwner.emplace(fixture.routes.implementation.routeTable(), fixture.worker,
        fixture.services, fixture.options, outbound, currentRegistration->activation,
        ConnectionConfig{.epoch = kEpoch + 62,
            .connectionGeneration = kGeneration + 62,
            .maxTrackedStreams = 8});
    RUVIA_CHECK(scheduler.attach(currentRegistration->token, *currentOwner));

    Connection::WorkerActivation stale{
        .work = {.runnable = {.data = true}}};
    oldRegistration->activation.activate(oldRegistration->activation.context,
        oldRegistration->token.epoch, oldRegistration->token.connectionGeneration,
        oldRegistration->token.slotGeneration, stale);
    const auto state = scheduler.snapshot();
    RUVIA_CHECK_EQ(state.runnable[0], std::size_t{0});
    RUVIA_CHECK_EQ(state.attachedConnections, std::size_t{1});

    RUVIA_CHECK(currentOwner->requestStop());
    const auto currentClose = scheduler.step();
    RUVIA_CHECK(currentClose.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(scheduler.beginRetirement(currentRegistration->token));
    co_await currentOwner->join();
    RUVIA_CHECK(currentOwner->takeOverTransportRetirement(
        {.epoch = currentRegistration->token.epoch,
            .connectionGeneration = currentRegistration->token.connectionGeneration}));
    RUVIA_CHECK(scheduler.acknowledgeIntent(
        currentRegistration->token, currentClose.intent.token));
    RUVIA_CHECK(scheduler.retire(currentRegistration->token));
    currentOwner.reset();
    RUVIA_CHECK(outbound.stop());
}

ruvia::Task<void> exerciseIntentDoesNotChangeArmedMailboxInterest(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    CrossThreadWake wake;
    CapacitySignal capacity({.context = &wake, .notify = &CrossThreadWake::notify});
    Scheduler scheduler(worker, 2, fixture.worker.resource(), &capacity);
    Mailbox outbound(1, 1, 1, fixture.worker.resource(), capacity.notifier());
    RUVIA_CHECK(scheduler.bindMailbox(outbound));
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    std::array<std::optional<Connection>, 2> owners;
    std::array<Scheduler::ConnectionToken, 2> tokens{};
    const std::array<std::byte, 1> filler{std::byte{0x42}};
    const MessageId fillerId{kEpoch + 130, kGeneration + 130, 0};
    RUVIA_CHECK(accepted(outbound.trySend(fillerId, filler)));
    RUVIA_CHECK(accepted(outbound.trySendControl(
        {.kind = Control::Kind::kWritable, .id = fillerId})));

    for (std::size_t i = 0; i < owners.size(); ++i) {
        const auto epoch = kEpoch + 131 + i;
        const auto generation = kGeneration + 131 + i;
        const auto registration = scheduler.reserve(epoch, generation);
        if (!registration) {
            throw std::runtime_error("scheduler interest-change slot unavailable");
        }
        tokens[i] = registration->token;
        owners[i].emplace(fixture.routes.implementation.routeTable(), fixture.worker,
            fixture.services, fixture.options, outbound, registration->activation,
            ConnectionConfig{.epoch = epoch,
                .connectionGeneration = generation,
                .maxTrackedStreams = 8});
        RUVIA_CHECK(scheduler.attach(tokens[i], *owners[i]));
    }
    const auto request = feedRequest(*owners[0], inbound, fixture,
        {tokens[0].epoch, tokens[0].connectionGeneration, 0}, "GET", "/small");
    RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
    std::array<Connection*, 1> references{&*owners[0]};
    co_await waitForReady(references, worker, fixture.stopToken);
    const auto blocked = scheduler.step();
    RUVIA_CHECK(blocked.kind == Scheduler::StepKind::kPublication);
    RUVIA_CHECK(blocked.publication.publication.blockReason ==
                Connection::Dispatch::PublishBlockReason::kData);
    const auto dataWait = scheduler.armCapacityWait();
    RUVIA_CHECK(dataWait.status == Scheduler::CapacityArmStatus::kArmed);
    RUVIA_CHECK(dataWait.interest == Mailbox::CapacityInterest::kData);

    const auto malformed = feedMalformedHeaders(*owners[1], inbound, fixture,
        {tokens[1].epoch, tokens[1].connectionGeneration, 0});
    RUVIA_CHECK(malformed.status == Connection::EventStatus::kProtocolError);
    const auto reset = scheduler.step();
    RUVIA_CHECK(reset.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(scheduler.parkIntentForControlCapacity(reset.connection, reset.intent.token));
    const auto bothWait = scheduler.armCapacityWait();
    RUVIA_CHECK(bothWait.status == Scheduler::CapacityArmStatus::kArmed);
    RUVIA_CHECK(bothWait.interest == Mailbox::CapacityInterest::kData);

    std::atomic<bool> consumerDone{};
    std::thread dataConsumer([&] {
        Mailbox::BorrowedBlock block;
        if (!outbound.tryReceive(block)) {
            std::terminate();
        }
        block.release();
        Control control;
        if (!outbound.tryReceiveControl(control) || outbound.finishDrain()) {
            std::terminate();
        }
        consumerDone.store(true, std::memory_order_release);
    });
    co_await waitForAtomic(consumerDone, worker, fixture.stopToken);
    dataConsumer.join();
    RUVIA_CHECK_EQ(wake.notifications.load(std::memory_order_acquire), 1U);
    scheduler.notifyTransportCapacity();
    auto resumed = scheduler.step();
    std::size_t turn = 0;
    while (turn++ < 3 && resumed.kind != Scheduler::StepKind::kTransportIntent &&
           resumed.kind != Scheduler::StepKind::kIdle) {
        resumed = scheduler.step();
    }
    RUVIA_CHECK(resumed.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(resumed.intent.token == reset.intent.token);
    RUVIA_CHECK(resumed.intent.streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kMessageError);
    RUVIA_CHECK(accepted(outbound.trySendControl({.kind = Control::Kind::kStreamReset,
        .id = resumed.intent.token.id,
        .streamResetErrorCode = resumed.intent.streamResetErrorCode})));
    RUVIA_CHECK(scheduler.acknowledgeIntent(resumed.connection, resumed.intent.token));
    Control received;
    RUVIA_CHECK(outbound.tryReceiveControl(received));
    RUVIA_CHECK(received.streamResetErrorCode ==
                ruvia::Http3ConnectionErrorCode::kMessageError);
    Mailbox::BorrowedBlock block;
    RUVIA_CHECK(outbound.tryReceive(block));
    block.release();
    (void)outbound.drainReturns();
    RUVIA_CHECK(!outbound.finishDrain());
    for (std::size_t i = 0; i < owners.size(); ++i) {
        co_await stopAndRetire(scheduler, tokens[i], *owners[i], ruvia_ctx);
        owners[i].reset();
    }
    (void)scheduler.takeLocalWakeObligation();
    RUVIA_CHECK(outbound.stop());
}

ruvia::Task<void> exerciseControlBurstOnePreservesIntentTurn(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    Scheduler scheduler(worker, 4, fixture.worker.resource(), {}, 1);
    Mailbox outbound(16, 16, 16, fixture.worker.resource());
    RUVIA_CHECK(scheduler.bindMailbox(outbound));
    Mailbox inbound(1, 1, 1, fixture.worker.resource());
    std::array<std::optional<Connection>, 4> owners;
    std::array<Scheduler::Registration, 4> registrations{};
    for (std::size_t i = 0; i < owners.size(); ++i) {
        const auto epoch = kEpoch + 150 + i;
        const auto generation = kGeneration + 150 + i;
        const auto registration = scheduler.reserve(epoch, generation);
        if (!registration) {
            throw std::runtime_error("scheduler control-burst slot unavailable");
        }
        registrations[i] = *registration;
        owners[i].emplace(fixture.routes.implementation.routeTable(), fixture.worker,
            fixture.services, fixture.options, outbound, registration->activation,
            ConnectionConfig{.epoch = epoch,
                .connectionGeneration = generation,
                .maxTrackedStreams = 8});
        RUVIA_CHECK(scheduler.attach(registration->token, *owners[i]));
    }
    for (std::size_t i = 0; i < 3; ++i) {
        const auto& token = registrations[i].token;
        const auto request = feedRequest(*owners[i], inbound, fixture,
            {token.epoch, token.connectionGeneration, 0}, i == 2 ? "GET" : "HEAD",
            i == 2 ? "/large" : "/head");
        RUVIA_CHECK(request.status == Connection::EventStatus::kDispatched);
    }
    std::array<Connection*, 3> ready{&*owners[0], &*owners[1], &*owners[2]};
    co_await waitForReady(ready, worker, fixture.stopToken);
    for (std::size_t i = 0; i < 2; ++i) {
        const auto head = owners[i]->publishOne({.data = true});
        RUVIA_CHECK(head.publication.status ==
                    Connection::Dispatch::PublishStatus::kBytesPublished);
        const auto& registration = registrations[i];
        registration.activation.activate(registration.activation.context,
            registration.token.epoch, registration.token.connectionGeneration,
            registration.token.slotGeneration, {.work = owners[i]->workState()});
    }
    const auto& errorToken = registrations[3].token;
    const auto malformed = feedMalformedHeaders(*owners[3], inbound, fixture,
        {errorToken.epoch, errorToken.connectionGeneration, 0});
    RUVIA_CHECK(malformed.status == Connection::EventStatus::kProtocolError);
    const auto initially = scheduler.snapshot();
    RUVIA_CHECK(initially.runnable[0] != 0);
    RUVIA_CHECK(initially.runnable[1] >= 2);
    RUVIA_CHECK(initially.runnable[3] != 0);
    const auto data = scheduler.step();
    const auto control = scheduler.step();
    const auto forcedData = scheduler.step();
    RUVIA_CHECK(data.kind == Scheduler::StepKind::kPublication);
    RUVIA_CHECK(control.kind == Scheduler::StepKind::kPublication);
    RUVIA_CHECK(forcedData.kind == Scheduler::StepKind::kPublication);
    RUVIA_CHECK(scheduler.snapshot().runnable[1] != 0);
    const auto intent = scheduler.step();
    RUVIA_CHECK(intent.kind == Scheduler::StepKind::kTransportIntent);
    RUVIA_CHECK(intent.intent.token.kind == Connection::TransportIntentKind::kStreamReset);
    RUVIA_CHECK(intent.connection == errorToken);
    RUVIA_CHECK(scheduler.acknowledgeIntent(intent.connection, intent.intent.token));

    for (std::size_t i = 0; i < owners.size(); ++i) {
        co_await stopAndRetire(scheduler, registrations[i].token, *owners[i], ruvia_ctx);
        owners[i].reset();
    }
    Control controlEvent;
    while (outbound.tryReceiveControl(controlEvent)) {
    }
    Mailbox::BorrowedBlock block;
    while (outbound.tryReceive(block)) {
        block.release();
    }
    (void)outbound.drainReturns();
    RUVIA_CHECK(!outbound.finishDrain());
    (void)scheduler.takeLocalWakeObligation();
    RUVIA_CHECK(outbound.stop());
}

ruvia::Task<void> exerciseCapacitySignalSurvivesScheduler(Fixture& fixture,
    const ruvia::WorkerHandle& worker, ruvia::testing::TestContext& ruvia_ctx) {
    CrossThreadWake wake;
    CapacitySignal capacity({.context = &wake, .notify = &CrossThreadWake::notify});
    {
        Scheduler scheduler(worker, 1, fixture.worker.resource(), &capacity);
        Mailbox outbound(1, 1, 1, fixture.worker.resource(), capacity.notifier());
        RUVIA_CHECK(scheduler.bindMailbox(outbound));
        RUVIA_CHECK(outbound.stop());
    }
    const auto before = capacity.generation();
    const auto notifier = capacity.notifier();
    notifier.notify(notifier.context);
    RUVIA_CHECK_EQ(capacity.generation(), before + 1);
    RUVIA_CHECK_EQ(wake.notifications.load(std::memory_order_acquire), 1U);
    co_return;
}

struct CapacityCallbackObservation final {
    std::binary_semaphore entered{0};
    std::binary_semaphore allowReturn{0};
    std::atomic<unsigned> notificationCalls{};
    std::atomic<unsigned> callbackReturns{};
    std::atomic<bool> callbackWaitExpired{};
    std::atomic<bool> signalDestroyed{};
    std::atomic<bool> wakeDestroyed{};
};

struct BlockingCapacityWake final {
    CapacityCallbackObservation& observation;

    ~BlockingCapacityWake() {
        observation.wakeDestroyed.store(true, std::memory_order_release);
    }

    static void notify(void* context) noexcept {
        auto& wake = *static_cast<BlockingCapacityWake*>(context);
        auto& observation = wake.observation;
        const auto call = observation.notificationCalls.fetch_add(
                              1, std::memory_order_acq_rel) +
                          1;
        if (call == 1) {
            observation.entered.release();
            if (!observation.allowReturn.try_acquire_for(3s)) {
                observation.callbackWaitExpired.store(true, std::memory_order_release);
            }
        }
        observation.callbackReturns.fetch_add(1, std::memory_order_release);
    }
};

struct CapacitySignalOwner final {
    CapacityCallbackObservation& observation;
    BlockingCapacityWake wake;
    std::optional<CapacitySignal> signal;

    explicit CapacitySignalOwner(CapacityCallbackObservation& state)
        : observation(state),
          wake{state},
          signal(std::in_place, CapacitySignal::WakeRef{
                                    .context = &wake,
                                    .notify = &BlockingCapacityWake::notify}) {}

    ~CapacitySignalOwner() {
        signal.reset();
        observation.signalDestroyed.store(true, std::memory_order_release);
    }
};

ruvia::Task<void> exerciseClaimedCapacityCallbackOutlivesStopAndScheduler(
    Fixture& fixture, const ruvia::WorkerHandle& worker,
    ruvia::testing::TestContext& ruvia_ctx) {
    constexpr auto kWatchdog = 3s;
    CapacityCallbackObservation observation;
    std::atomic<bool> receivedBlock{};
    std::atomic<bool> receiverFinished{};
    {
        CapacitySignalOwner owner(observation);
        Mailbox outbound(1, 1, 1, fixture.worker.resource(), owner.signal->notifier());
        const std::array<std::byte, 1> payload{std::byte{0x5a}};
        RUVIA_CHECK(outbound.trySend({kEpoch + 170, kGeneration + 170, 0}, payload) ==
                    Mailbox::SendResult::kSentNotifyPeer);
        RUVIA_CHECK(outbound.armCapacityWait(Mailbox::CapacityInterest::kData) ==
                    Mailbox::CapacityWaitResult::kArmed);

        std::thread capacityNotifier;
        {
            Scheduler scheduler(worker, 1, fixture.worker.resource(), &*owner.signal);
            RUVIA_CHECK(scheduler.bindMailbox(outbound));
            capacityNotifier = std::thread([&] {
                Mailbox::BorrowedBlock block;
                const bool received = outbound.tryReceive(block);
                receivedBlock.store(received, std::memory_order_release);
                block.release();
                receiverFinished.store(true, std::memory_order_release);
            });

            const bool callbackEntered = observation.entered.try_acquire_for(kWatchdog);
            RUVIA_CHECK(callbackEntered);
            if (callbackEntered) {
                RUVIA_CHECK_EQ(observation.notificationCalls.load(std::memory_order_acquire), 1U);
                RUVIA_CHECK_EQ(observation.callbackReturns.load(std::memory_order_acquire), 0U);
                RUVIA_CHECK_EQ(owner.signal->generation(), 1U);
            }
            RUVIA_CHECK(outbound.stop());
            RUVIA_CHECK(!receiverFinished.load(std::memory_order_acquire));
            RUVIA_CHECK(!observation.signalDestroyed.load(std::memory_order_acquire));
            RUVIA_CHECK(!observation.wakeDestroyed.load(std::memory_order_acquire));
        }

        // stop() closed the mailbox and the worker scheduler is gone, but it did
        // not join the thread whose already-claimed callback is still in WakeRef.
        RUVIA_CHECK_EQ(observation.notificationCalls.load(std::memory_order_acquire), 1U);
        RUVIA_CHECK_EQ(observation.callbackReturns.load(std::memory_order_acquire), 0U);
        RUVIA_CHECK(!receiverFinished.load(std::memory_order_acquire));
        RUVIA_CHECK(!observation.signalDestroyed.load(std::memory_order_acquire));
        RUVIA_CHECK(!observation.wakeDestroyed.load(std::memory_order_acquire));

        observation.allowReturn.release();
        capacityNotifier.join();
        RUVIA_CHECK(receivedBlock.load(std::memory_order_acquire));
        RUVIA_CHECK(receiverFinished.load(std::memory_order_acquire));
        RUVIA_CHECK(!observation.callbackWaitExpired.load(std::memory_order_acquire));
        RUVIA_CHECK_EQ(observation.callbackReturns.load(std::memory_order_acquire), 1U);
        RUVIA_CHECK_EQ(observation.notificationCalls.load(std::memory_order_acquire), 1U);
        RUVIA_CHECK_EQ(owner.signal->generation(), 1U);
        RUVIA_CHECK(!observation.signalDestroyed.load(std::memory_order_acquire));
        RUVIA_CHECK(!observation.wakeDestroyed.load(std::memory_order_acquire));
    }
    RUVIA_CHECK(observation.signalDestroyed.load(std::memory_order_acquire));
    RUVIA_CHECK(observation.wakeDestroyed.load(std::memory_order_acquire));
    co_return;
}

}  // namespace

RUVIA_TEST(http3_worker_mailbox_scheduler_waits_for_intent_ack_before_retirement) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exercise_retirement_waits_for_intent_acknowledgement(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3WorkerMailboxSchedulerRotatesConnectionsAndPublicationLanes) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseCrossConnectionLaneRotation(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3WorkerMailboxSchedulerFansAnyCapacityWakeAcrossBlockedConnections) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseCapacityWakeFansOutAndPreservesReset(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3WorkerMailboxSchedulerCapacitySignalOutlivesScheduler) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseCapacitySignalSurvivesScheduler(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3WorkerMailboxSchedulerKeepsClaimedCapacityCallbackAliveAcrossStop) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseClaimedCapacityCallbackOutlivesStopAndScheduler(
                fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3WorkerMailboxSchedulerRecoversControlOnlyResetOnTransportCapacity) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseControlOnlyResetCapacityWait(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3WorkerMailboxSchedulerKeepsIntentOutOfMailboxCapacityInterest) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseIntentDoesNotChangeArmedMailboxInterest(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3WorkerMailboxSchedulerControlBurstOnePreservesIntentTurn) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseControlBurstOnePreservesIntentTurn(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3WorkerMailboxSchedulerKeepsLocalDeadlineActivationWithoutCapacityNotifier) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseLocalDeadlineWithoutCapacityNotifier(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3WorkerMailboxSchedulerRejectsStaleActivationAfterJoinedSlotReuse) {
    auto& io = ruvia::test::newTestIoContext();
    auto attachment = ruvia::attachEventLoop(io, {.mailboxCapacity = 32});
    const auto worker = attachment.loop().handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        runWorkerTask(attachment,
            exerciseStaleSlotActivationAfterJoin(fixture, worker, ruvia_ctx));
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

ruvia::Task<void> runActivationWakeWindows(ruvia::WorkerRuntimeContext& runtime,
    Fixture& fixture, ruvia::testing::TestContext& ruvia_ctx) {
    co_await exerciseActivationWakeWindow(runtime, fixture, true, ruvia_ctx);
    co_await exerciseActivationWakeWindow(runtime, fixture, false, ruvia_ctx);
    runtime.close();
}

RUVIA_TEST(http3WorkerMailboxSchedulerWakesLocalActivationBeforeAndAfterWaitArm) {
    asio::io_context io;
    ruvia::WorkerRuntimeContext runtime(io, 32);
    const auto worker = runtime.handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        std::promise<std::exception_ptr> taskExitPromise;
        auto taskExit = taskExitPromise.get_future();
        asio::co_spawn(io,
            ruvia::asAwaitable(runActivationWakeWindows(runtime, fixture, ruvia_ctx)),
            [&taskExitPromise](std::exception_ptr failure) {
                taskExitPromise.set_value(std::move(failure));
            });
        std::promise<std::exception_ptr> runtimeExitPromise;
        auto runtimeExit = runtimeExitPromise.get_future();
        std::thread workerThread([&] {
            std::exception_ptr failure;
            try {
                runtime.run();
            } catch (...) {
                failure = std::current_exception();
            }
            runtimeExitPromise.set_value(std::move(failure));
        });
        RUVIA_CHECK(taskExit.wait_for(5s) == std::future_status::ready);
        if (taskExit.wait_for(0s) == std::future_status::ready) {
            RUVIA_CHECK(taskExit.get() == nullptr);
        }
        RUVIA_CHECK(runtimeExit.wait_for(5s) == std::future_status::ready);
        workerThread.join();
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3WorkerServerOwnerRollsBackBeforeLaunch) {
    asio::io_context io;
    ruvia::WorkerRuntimeContext runtime(io, 32);
    const auto worker = runtime.handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        Mailbox requestMailbox(1, 1, 1);
        CrossThreadWake networkWake;
        WorkerServerWakeBridge workerWake;
        ConnectionChannel channel(
            {.context = &networkWake, .notify = &CrossThreadWake::notify},
            {.context = &workerWake, .notify = &WorkerServerWakeBridge::notify});
        std::promise<std::exception_ptr> taskExitPromise;
        auto taskExit = taskExitPromise.get_future();
        asio::co_spawn(io,
            ruvia::asAwaitable(exerciseWorkerServerPreLaunchRollback(
                runtime, fixture, requestMailbox, channel, networkWake, ruvia_ctx)),
            [&taskExitPromise](std::exception_ptr failure) {
                taskExitPromise.set_value(std::move(failure));
            });
        std::promise<std::exception_ptr> runtimeExitPromise;
        auto runtimeExit = runtimeExitPromise.get_future();
        std::thread workerThread([&] {
            std::exception_ptr failure;
            try {
                runtime.run();
            } catch (...) {
                failure = std::current_exception();
            }
            runtimeExitPromise.set_value(std::move(failure));
        });
        RUVIA_CHECK(taskExit.wait_for(5s) == std::future_status::ready);
        if (taskExit.wait_for(0s) == std::future_status::ready) {
            RUVIA_CHECK(taskExit.get() == nullptr);
        }
        RUVIA_CHECK(runtimeExit.wait_for(5s) == std::future_status::ready);
        workerThread.join();
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}

RUVIA_TEST(http3WorkerServerRunLoopReceivesNetworkCapacityNotifications) {
    asio::io_context io;
    ruvia::WorkerRuntimeContext runtime(io, 32);
    const auto worker = runtime.handle();
    ruvia::test::CountingMemoryResource upstream;
    {
        Fixture fixture(worker, upstream);
        Mailbox requestMailbox(1, 1, 1);
        CrossThreadWake networkWake;
        WorkerServerWakeBridge workerWake;
        ConnectionChannel channel(
            {.context = &networkWake, .notify = &CrossThreadWake::notify},
            {.context = &workerWake, .notify = &WorkerServerWakeBridge::notify});
        std::atomic<bool> installed{};
        std::atomic<bool> stopRequested{};
        std::promise<std::exception_ptr> taskExitPromise;
        auto taskExit = taskExitPromise.get_future();
        asio::co_spawn(io, ruvia::asAwaitable(runProductionWorkerServer(runtime, fixture, requestMailbox, channel, networkWake, workerWake, installed, stopRequested, ruvia_ctx)),
            [&taskExitPromise](std::exception_ptr failure) {
                taskExitPromise.set_value(std::move(failure));
            });
        std::promise<std::exception_ptr> runtimeExitPromise;
        auto runtimeExit = runtimeExitPromise.get_future();
        std::thread workerThread([&] {
            std::exception_ptr failure;
            try {
                runtime.run();
            } catch (...) {
                failure = std::current_exception();
            }
            runtimeExitPromise.set_value(std::move(failure));
        });

        const auto deadline = std::chrono::steady_clock::now() + 5s;
        ConnectionChannel::Identity identity;
        bool grantReceived = false;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto status = channel.peekGrant(identity);
            if (status == ConnectionChannel::Status::kReceived) {
                grantReceived = true;
                break;
            }
            if (status != ConnectionChannel::Status::kEmpty) {
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        // Grant is published inside install(), before the parent task stores
        // installed after spawning run(). Observe both milestones separately.
        while (!installed.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        RUVIA_CHECK(installed.load(std::memory_order_acquire));
        RUVIA_CHECK(grantReceived);
        if (grantReceived) {
            RUVIA_CHECK(channel.revokeGrant() == ConnectionChannel::Status::kPublished);
            ConnectionChannel::RevokeAck revokeAck;
            bool revokeReceived = false;
            while (std::chrono::steady_clock::now() < deadline) {
                const auto status = channel.receiveRevokeAck(revokeAck);
                if (status == ConnectionChannel::Status::kReceived) {
                    revokeReceived = revokeAck.identity == identity;
                    break;
                }
                if (status != ConnectionChannel::Status::kEmpty) {
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }
            RUVIA_CHECK(revokeReceived);
            auto closeStatus = ConnectionChannel::Status::kWrongState;
            while (revokeReceived && std::chrono::steady_clock::now() < deadline) {
                closeStatus = channel.closeNetworkPublications(identity);
                if (closeStatus == ConnectionChannel::Status::kPublished) {
                    break;
                }
                if (closeStatus != ConnectionChannel::Status::kWrongState) {
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }
            RUVIA_CHECK(closeStatus == ConnectionChannel::Status::kPublished);
        }
        stopRequested.store(true, std::memory_order_release);
        if (auto* server = workerWake.server.load(std::memory_order_acquire);
            server != nullptr) {
            (void)server->notification().notify();
        }
        RUVIA_CHECK(taskExit.wait_for(5s) == std::future_status::ready);
        if (taskExit.wait_for(0s) == std::future_status::ready) {
            RUVIA_CHECK(taskExit.get() == nullptr);
        }
        runtime.close();
        RUVIA_CHECK(runtimeExit.wait_for(5s) == std::future_status::ready);
        workerThread.join();
        RUVIA_CHECK(channel.readyToDestroy());
        RUVIA_CHECK(workerWake.notifications.load(std::memory_order_acquire) >= 2U);
    }
    RUVIA_CHECK_EQ(upstream.liveAllocations(), std::size_t{0});
    RUVIA_CHECK_EQ(upstream.allocationCount(), upstream.deallocationCount());
}
