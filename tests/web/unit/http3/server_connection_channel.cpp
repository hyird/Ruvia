#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory_resource>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <thread>
#include <utility>

#include <asio/io_context.hpp>

#include "ruvia/core/EventLoopAttachment.h"
#include "ruvia/core/WorkerHandle.h"
#include "ruvia/core/memory/MemoryPool.h"
#include "ruvia/web/Context.h"
#include "ruvia/web/detail/CallbackRef.h"
#include "ruvia/web/detail/http3/Http3ServerConnection.h"
#include "ruvia/web/detail/http3/Http3ServerConnectionChannel.h"
#include "ruvia/web/detail/http3/Http3WorkerMailboxScheduler.h"
#include "ruvia/web/detail/router/Router.h"
#include "ruvia/web/detail/router/RouterImpl.h"

#include "memory_resource_fixture.h"
#include "test_harness.h"

namespace {

using Channel = ruvia::detail::Http3ServerConnectionChannel;
using Connection = ruvia::detail::Http3ServerConnection;
using Scheduler = ruvia::detail::Http3WorkerMailboxScheduler;
using Status = Channel::Status;

class CountingResource final : public std::pmr::memory_resource {
public:
    std::size_t allocated{};
    std::size_t freed{};
    std::size_t allocations{};
    std::size_t deallocations{};

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        void* result = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        allocated += bytes;
        ++allocations;
        return result;
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        freed += bytes;
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct Wake final {
    std::atomic<std::uint32_t> calls{};
    std::atomic<bool> blockNext{};
    std::binary_semaphore entered{0};
    std::binary_semaphore release{0};

    static void notify(void* context) noexcept {
        auto& wake = *static_cast<Wake*>(context);
        wake.calls.fetch_add(1, std::memory_order_relaxed);
        if (wake.blockNext.exchange(false, std::memory_order_acq_rel)) {
            wake.entered.release();
            wake.release.acquire();
        }
    }

    [[nodiscard]] Channel::Notification target() noexcept {
        return {.context = this, .notify = &notify};
    }
};

struct Routes final {
    ruvia::detail::Router router;
    ruvia::detail::RouterImpl& implementation{ruvia::detail::RouterImpl::from(router)};

    Routes() {
        implementation.finalize();
    }
};

struct Runtime final {
    Routes routes;
    ruvia::WorkerMemory workerMemory;
    ruvia::StopSource stopSource;
    ruvia::StopToken stopToken;
    ruvia::detail::ContextServices services;
    ruvia::detail::HttpServerOptions options;
    ruvia::detail::Http3StreamMailbox outbound;
    std::optional<Connection> connection;

    Runtime(const ruvia::WorkerHandle& workerHandle, std::pmr::memory_resource& resource)
        : workerMemory(resource),
          stopToken(stopSource.token()),
          services(workerHandle, stopToken),
          outbound(8, 8, 8, workerMemory.resource()) {}
};

struct AsyncCompletion final {
    std::binary_semaphore done{0};
    std::exception_ptr error;
};

class TestWorker final {
public:
    explicit TestWorker(std::pmr::memory_resource& resource)
        : attachment_(ruvia::attachEventLoop(io_, {.mailboxCapacity = 16})),
          handle_(attachment_.loop().handle()),
          resource_(resource) {
        auto posted = handle_.post([this] {
            runtime_.emplace(handle_, resource_);
            scheduler_.emplace(handle_, 1, runtime_->workerMemory.resource());
            initialized_.release();
        });
        if (!posted.accepted()) {
            throw std::runtime_error("failed to initialize HTTP/3 test worker");
        }
        thread_ = std::thread([this] { attachment_.run(); });
        initialized_.acquire();
    }

    ~TestWorker() {
        if (!thread_.joinable()) {
            return;
        }
        std::binary_semaphore done(0);
        auto posted = handle_.post([this, &done] {
            if (runtime_->connection) {
                std::terminate();
            }
            scheduler_.reset();
            if (!runtime_->outbound.stop()) {
                std::terminate();
            }
            runtime_.reset();
            attachment_.stop();
            done.release();
        });
        if (!posted.accepted()) {
            std::terminate();
        }
        done.acquire();
        thread_.join();
    }

    TestWorker(const TestWorker&) = delete;
    TestWorker& operator=(const TestWorker&) = delete;

    template <typename Fn>
    void call(Fn&& function) {
        std::binary_semaphore done(0);
        std::exception_ptr error;
        auto posted = handle_.post([this, &function, &done, &error] {
            try {
                function(*scheduler_, *runtime_);
            } catch (...) {
                error = std::current_exception();
            }
            done.release();
        });
        if (!posted.accepted()) {
            throw std::runtime_error("HTTP/3 test worker rejected a task");
        }
        done.acquire();
        if (error) {
            std::rethrow_exception(error);
        }
    }

    template <typename Fn>
    void callTask(Fn&& function) {
        AsyncCompletion completion;
        auto posted = handle_.post([this, &completion, function = std::forward<Fn>(function)]() mutable {
            root_.emplace(attachment_.loop().start(
                runTask(std::move(function), *scheduler_, *runtime_, completion)));
        });
        if (!posted.accepted()) {
            throw std::runtime_error("HTTP/3 test worker rejected an async task");
        }
        completion.done.acquire();
        root_->get();
        root_.reset();
        if (completion.error) {
            std::rethrow_exception(completion.error);
        }
    }

private:
    template <typename Fn>
    static ruvia::Task<void> runTask(Fn function, Scheduler& scheduler,
        Runtime& runtime, AsyncCompletion& completion) {
        try {
            co_await function(scheduler, runtime);
        } catch (...) {
            completion.error = std::current_exception();
        }
        completion.done.release();
    }

    asio::io_context io_;
    ruvia::EventLoopAttachment attachment_;
    ruvia::WorkerHandle handle_;
    std::pmr::memory_resource& resource_;
    std::optional<Runtime> runtime_;
    std::optional<Scheduler> scheduler_;
    std::optional<ruvia::RootTask<void>> root_;
    std::binary_semaphore initialized_{0};
    std::thread thread_;
};

class TestNetworkTransport final {
public:
    void queueAccept() noexcept {
        pending_ = true;
    }

    [[nodiscard]] std::optional<std::uint64_t> acceptConnections(std::size_t credits) noexcept {
        if (credits != 1 || !pending_) {
            return std::nullopt;
        }
        pending_ = false;
        active_ = nextId_++;
        return active_;
    }

    [[nodiscard]] bool retireConnectionLocally(std::uint64_t id) noexcept {
        if (active_ != id || id == 0) {
            return false;
        }
        active_ = 0;
        return true;
    }

private:
    std::uint64_t nextId_{101};
    std::uint64_t active_{};
    bool pending_{};
};

Channel::TransportIntent resetIntent(Channel::Identity identity, std::uint64_t streamId,
    std::uint64_t sequence, ruvia::Http3ConnectionErrorCode errorCode);

struct ChannelFixture final {
    Wake networkWake;
    Wake workerWake;
    Channel channel;
    TestWorker worker;
    TestNetworkTransport transport;
    Channel::GrantPublication grant{};
    std::optional<std::uint64_t> acceptedId;

    explicit ChannelFixture(std::pmr::memory_resource& memory, bool datagrams = false)
        : channel(networkWake.target(), workerWake.target(), datagrams ? &memory : nullptr),
          worker(memory) {}

    void publishGrant(std::uint64_t epoch, std::uint64_t generation) {
        worker.call([&](Scheduler& scheduler, Runtime&) {
            grant = channel.reserveAndPublishGrant(scheduler, epoch, generation);
        });
        if (grant.status != Status::kPublished) {
            throw std::runtime_error("HTTP/3 test scheduler could not reserve a Grant");
        }
    }

    void acceptAndCommit() {
        acceptedId = transport.acceptConnections(1);
        if (!acceptedId) {
            throw std::runtime_error("test transport has no queued accepted connection");
        }
        if (channel.commitAccepted(grant.identity) != Status::kPublished) {
            throw std::runtime_error("HTTP/3 channel rejected an accepted test connection");
        }
        Channel::AttachResult prematureAck{std::in_place_type<Channel::AttachAck>};
        if (channel.receiveAttachResult(prematureAck) != Status::kEmpty) {
            throw std::runtime_error("HTTP/3 attach result appeared before scheduler.attach");
        }
    }

    void retireTransport() {
        if (!acceptedId) {
            throw std::runtime_error("test transport has no accepted connection to retire");
        }
        const auto workerWakeBefore = workerWake.calls.load(std::memory_order_relaxed);
        if (!transport.retireConnectionLocally(*acceptedId)) {
            throw std::runtime_error("test transport did not physically retire the connection");
        }
        if (channel.publishTransportRetired(grant.identity) != Status::kPublished ||
            workerWake.calls.load(std::memory_order_relaxed) <= workerWakeBefore) {
            throw std::runtime_error("physical transport retirement was not reliably published");
        }
        const auto workerWakeAfterPublish = workerWake.calls.load(std::memory_order_relaxed);
        if (channel.publishTransportRetired(grant.identity) != Status::kWrongState ||
            workerWake.calls.load(std::memory_order_relaxed) != workerWakeAfterPublish) {
            throw std::runtime_error("duplicate transport retirement was published");
        }
    }

    void bindAndAttach(bool publishCloseBeforeNetworkAck = false) {
        Channel::Bind bind;
        Status bindStatus{};
        Status attachStatus{};
        Status closePublicationStatus{Status::kWrongState};
        worker.callTask([&](Scheduler& scheduler, Runtime& runtime) -> ruvia::Task<void> {
            if (channel.reject(scheduler, grant.registration, Channel::RejectReason::kStopping) != Status::kWrongState) {
                throw std::runtime_error("HTTP/3 rejection bypassed Bind consumption");
            }
            bindStatus = channel.receiveBind(bind);
            Channel::Bind duplicate;
            if (bindStatus != Status::kReceived || bind.identity != grant.identity ||
                channel.receiveBind(duplicate) != Status::kEmpty) {
                throw std::runtime_error("HTTP/3 worker did not consume Bind exactly once");
            }
            runtime.connection.emplace(runtime.routes.implementation.routeTable(),
                runtime.workerMemory, runtime.services, runtime.options, runtime.outbound,
                grant.registration.activation,
                ruvia::detail::Http3ServerConnectionConfig{
                    .epoch = grant.identity.epoch,
                    .connectionGeneration = grant.identity.connectionGeneration,
                    .maxTrackedStreams = 8});
            attachStatus = channel.attach(scheduler, grant.registration, *runtime.connection);
            if (attachStatus != Status::kPublished ||
                channel.attach(scheduler, grant.registration, *runtime.connection) != Status::kWrongState ||
                channel.reject(scheduler, grant.registration, Channel::RejectReason::kStopping) != Status::kWrongState) {
                throw std::runtime_error("HTTP/3 attach outcome was not single-shot");
            }
            if (publishCloseBeforeNetworkAck) {
                if (!runtime.connection->requestStop()) {
                    throw std::runtime_error("HTTP/3 connection did not enter stop");
                }
                const auto pending = runtime.connection->peekTransportIntent();
                if (!pending || pending->token.kind !=
                                    Connection::TransportIntentKind::kConnectionClose) {
                    throw std::runtime_error("HTTP/3 close intent was not recorded after attach");
                }
                closePublicationStatus = channel.publishIntent(grant.identity, *pending);
            }
            co_return;
        });
        if (bindStatus != Status::kReceived || attachStatus != Status::kPublished ||
            (publishCloseBeforeNetworkAck && closePublicationStatus != Status::kPublished)) {
            throw std::runtime_error("HTTP/3 channel Bind/attach did not complete");
        }
    }

    void finishAttached(bool closeAlreadyPublished = false, std::uint64_t next_sequence = 1) {
        Connection::TransportIntent closeIntent;
        worker.callTask([&](Scheduler& scheduler, Runtime& runtime) -> ruvia::Task<void> {
            auto& connection = *runtime.connection;
            if (!closeAlreadyPublished && !connection.requestStop()) {
                throw std::runtime_error("HTTP/3 connection did not enter stop");
            }
            const auto pending = connection.peekTransportIntent();
            if (!pending || pending->token.kind != Connection::TransportIntentKind::kConnectionClose) {
                throw std::runtime_error("HTTP/3 connection close intent was not recorded");
            }
            closeIntent = *pending;
            if ((!closeAlreadyPublished &&
                    channel.publishIntent(grant.identity, closeIntent) != Status::kPublished) ||
                !connection.ackTransportIntent(closeIntent.token) ||
                !scheduler.beginRetirement(grant.registration.token)) {
                throw std::runtime_error("HTTP/3 close intent/retirement handoff failed");
            }
            co_await connection.join();
            if (!scheduler.retire(grant.registration.token)) {
                throw std::runtime_error("HTTP/3 scheduler did not retire its connection");
            }
            runtime.connection.reset();
        });

        Connection::TransportIntent networkOwned;
        if (channel.receiveIntent(networkOwned) != Status::kReceived ||
            networkOwned.token != closeIntent.token ||
            networkOwned.closeReason != closeIntent.closeReason ||
            networkOwned.connectionErrorCode != closeIntent.connectionErrorCode) {
            throw std::runtime_error("network did not retain the exact close intent");
        }
        if (channel.acknowledgeIntentAfterHandoff(grant.identity, networkOwned.token) !=
            Status::kPublished) {
            throw std::runtime_error("network could not acknowledge its close-intent handoff");
        }
        Channel::TransportIntentAck closeAck;
        Status closeAckStatus{};
        worker.call([&](Scheduler&, Runtime&) {
            closeAckStatus = channel.receiveIntentAck(closeAck);
        });
        if (closeAckStatus != Status::kReceived || closeAck.token != closeIntent.token) {
            throw std::runtime_error("worker did not receive the exact close-intent ACK");
        }
        std::array<Connection::TransportIntentToken, Channel::kControlCapacity> resetTokens{};
        std::size_t resetCount = 0;
        for (;;) {
            Connection::TransportIntent reset;
            const auto status = channel.receiveIntent(reset);
            if (status == Status::kEmpty) {
                break;
            }
            if (status != Status::kReceived || resetCount == resetTokens.size() ||
                reset.token.kind != Connection::TransportIntentKind::kStreamReset ||
                channel.acknowledgeIntentAfterHandoff(grant.identity, reset.token) !=
                    Status::kPublished) {
                throw std::runtime_error("queued reset did not receive exact network handoff");
            }
            resetTokens[resetCount++] = reset.token;
        }
        std::array<Status, Channel::kControlCapacity> resetAckStatuses{};
        std::array<Channel::TransportIntentAck, Channel::kControlCapacity> resetAcks{};
        worker.call([&](Scheduler&, Runtime&) {
            for (std::size_t i = 0; i < resetCount; ++i) {
                resetAckStatuses[i] = channel.receiveIntentAck(resetAcks[i]);
            }
        });
        for (std::size_t i = 0; i < resetCount; ++i) {
            if (resetAckStatuses[i] != Status::kReceived ||
                resetAcks[i].token != resetTokens[i] ||
                resetAcks[i].settlement != Channel::IntentSettlement::kExecutedHandoff) {
                throw std::runtime_error("worker did not receive exact reset-token ACK");
            }
        }
        const auto lateResetSequence = resetCount == 0
                                           ? next_sequence
                                           : std::max(next_sequence, resetTokens[resetCount - 1].sequence + 1);
        const auto lateResetIntent = resetIntent(grant.identity, 4, lateResetSequence,
            ruvia::Http3ConnectionErrorCode::kRequestCancelled);
        const Connection::TransportIntent latePushIntent{
            .token = {.kind = Connection::TransportIntentKind::kOpenPushStream,
                .id = {.epoch = grant.identity.epoch,
                    .connectionGeneration = grant.identity.connectionGeneration,
                    .streamId = 0,
                    .pushId = 0},
                .sequence = lateResetSequence + 1}};
        Status lateReset{};
        Status latePush{};
        Status repeatedClose{};
        worker.call([&](Scheduler&, Runtime&) {
            lateReset = channel.publishIntent(grant.identity, lateResetIntent);
            latePush = channel.publishIntent(grant.identity, latePushIntent);
            repeatedClose = channel.publishIntent(grant.identity, closeIntent);
        });
        if (lateReset != Status::kPublished || latePush != Status::kPublished ||
            repeatedClose != Status::kWrongState) {
            throw std::runtime_error("close ACK did not preserve owed intent publication");
        }
        Connection::TransportIntent lateResetReceived;
        Connection::TransportIntent latePushReceived;
        if (channel.receiveIntent(lateResetReceived) != Status::kReceived ||
            lateResetReceived.token != lateResetIntent.token ||
            channel.receiveIntent(latePushReceived) != Status::kReceived ||
            latePushReceived.token != latePushIntent.token) {
            throw std::runtime_error("network did not receive close-prioritized owed intents");
        }
        const auto finalResetIntent = resetIntent(grant.identity, 8, lateResetSequence + 2,
            ruvia::Http3ConnectionErrorCode::kRequestCancelled);
        Status finalResetPublication{};
        worker.call([&](Scheduler&, Runtime&) {
            finalResetPublication = channel.publishIntent(grant.identity, finalResetIntent);
        });
        if (finalResetPublication != Status::kPublished) {
            throw std::runtime_error("worker could not refill the owed intent lane");
        }
        Connection::TransportIntent finalResetReceived;
        if (channel.receiveIntent(finalResetReceived) != Status::kReceived ||
            finalResetReceived.token != finalResetIntent.token) {
            throw std::runtime_error("network did not receive the refilled reset intent");
        }
        const Connection::PushStreamOpenResult stoppedPush{
            .status = Connection::PushStreamOpenResult::Status::kStopped};
        if (channel.acknowledgeIntentAfterHandoff(grant.identity, lateResetReceived.token) !=
                Status::kPublished ||
            channel.acknowledgeIntentAfterHandoff(grant.identity, latePushReceived.token,
                Channel::IntentSettlement::kExecutedHandoff, stoppedPush) != Status::kPublished ||
            channel.acknowledgeIntentAfterHandoff(grant.identity, finalResetReceived.token) !=
                Status::kFull) {
            throw std::runtime_error("network could not preserve full owed-intent ACK state");
        }
        std::array<Channel::TransportIntentAck, 2> lateAcks{};
        std::array<Status, 2> lateAckStatuses{};
        worker.call([&](Scheduler&, Runtime&) {
            lateAckStatuses[0] = channel.receiveIntentAck(lateAcks[0]);
            lateAckStatuses[1] = channel.receiveIntentAck(lateAcks[1]);
        });
        if (lateAckStatuses[0] != Status::kReceived ||
            lateAcks[0].token != lateResetIntent.token ||
            lateAckStatuses[1] != Status::kReceived ||
            lateAcks[1].token != latePushIntent.token || !lateAcks[1].pushStream ||
            lateAcks[1].pushStream->status !=
                Connection::PushStreamOpenResult::Status::kStopped ||
            channel.acknowledgeIntentAfterHandoff(grant.identity, finalResetReceived.token) !=
                Status::kPublished) {
            throw std::runtime_error("worker did not receive exact post-close intent ACKs");
        }
        Channel::TransportIntentAck finalResetAck;
        Status finalResetAckStatus{};
        worker.call([&](Scheduler&, Runtime&) {
            finalResetAckStatus = channel.receiveIntentAck(finalResetAck);
        });
        if (finalResetAckStatus != Status::kReceived ||
            finalResetAck.token != finalResetIntent.token) {
            throw std::runtime_error("network did not retry the exact reset ACK");
        }
        Status beforePhysicalRetirement{};
        worker.call([&](Scheduler&, Runtime&) {
            Channel::TransportRetired retired;
            beforePhysicalRetirement = channel.receiveTransportRetired(retired);
        });
        if (beforePhysicalRetirement != Status::kEmpty) {
            throw std::runtime_error("worker observed retirement before transport release");
        }
        retireTransport();

        Status prematureClose{};
        Status retirementReceived{};
        Status duplicateRetirement{};
        Status workerClosed{};
        Status prematureFinalized{};
        Status workerFinalized{};
        Channel::TransportRetired retired;
        worker.call([&](Scheduler&, Runtime&) {
            prematureClose = channel.closeWorkerPublications(grant.identity);
            retirementReceived = channel.receiveTransportRetired(retired);
            Channel::TransportRetired duplicate;
            duplicateRetirement = channel.receiveTransportRetired(duplicate);
            workerClosed = channel.closeWorkerPublications(grant.identity);
            prematureFinalized = channel.publishWorkerFinalized(grant.identity);
        });
        if (prematureClose != Status::kWrongState ||
            retirementReceived != Status::kReceived || retired.identity != grant.identity ||
            duplicateRetirement != Status::kEmpty || workerClosed != Status::kPublished ||
            prematureFinalized != Status::kWrongState ||
            channel.closeNetworkPublications(grant.identity) != Status::kPublished) {
            throw std::runtime_error("HTTP/3 connection publication gates failed");
        }
        worker.call([&](Scheduler&, Runtime&) {
            workerFinalized = channel.publishWorkerFinalized(grant.identity);
        });
        if (workerFinalized != Status::kPublished ||
            channel.publishNetworkFinalized(grant.identity) != Status::kPublished) {
            throw std::runtime_error("HTTP/3 connection finalization controls failed");
        }
    }
};

Channel::TransportIntent resetIntent(Channel::Identity identity, std::uint64_t streamId,
    std::uint64_t sequence, ruvia::Http3ConnectionErrorCode errorCode) {
    return {.token = {.kind = Connection::TransportIntentKind::kStreamReset,
                .id = {.epoch = identity.epoch,
                    .connectionGeneration = identity.connectionGeneration,
                    .streamId = streamId},
                .sequence = sequence},
        .streamResetErrorCode = errorCode,
        .closeReason = Connection::TransportCloseReason::kNone,
        .connectionErrorCode = std::nullopt};
}

void finishRejected(ChannelFixture& fixture, std::uint64_t id) {
    if (!fixture.acceptedId || *fixture.acceptedId != id) {
        throw std::runtime_error("rejected test connection identity changed");
    }
    fixture.retireTransport();
    Status prematureClose{};
    Status retirementReceived{};
    Status duplicateRetirement{};
    Status workerClosed{};
    Status prematureFinalized{};
    Status workerFinalized{};
    Channel::TransportRetired retired;
    fixture.worker.call([&](Scheduler&, Runtime&) {
        prematureClose = fixture.channel.closeWorkerPublications(fixture.grant.identity);
        retirementReceived = fixture.channel.receiveTransportRetired(retired);
        Channel::TransportRetired duplicate;
        duplicateRetirement = fixture.channel.receiveTransportRetired(duplicate);
        workerClosed = fixture.channel.closeWorkerPublications(fixture.grant.identity);
        prematureFinalized = fixture.channel.publishWorkerFinalized(fixture.grant.identity);
    });
    if (prematureClose != Status::kWrongState || retirementReceived != Status::kReceived ||
        retired.identity != fixture.grant.identity || duplicateRetirement != Status::kEmpty ||
        workerClosed != Status::kPublished || prematureFinalized != Status::kWrongState ||
        fixture.channel.closeNetworkPublications(fixture.grant.identity) != Status::kPublished) {
        throw std::runtime_error("failed to close rejected test connection gates");
    }
    fixture.worker.call([&](Scheduler&, Runtime&) {
        workerFinalized = fixture.channel.publishWorkerFinalized(fixture.grant.identity);
    });
    if (workerFinalized != Status::kPublished ||
        fixture.channel.publishNetworkFinalized(fixture.grant.identity) != Status::kPublished) {
        throw std::runtime_error("failed to finalize rejected test connection");
    }
}

void acknowledgeFinalization(ChannelFixture& fixture) {
    Channel::WorkerFinalized workerFinalized;
    Channel::WorkerFinalized duplicate_worker;
    if (fixture.channel.acknowledgeWorkerFinalized(fixture.grant.identity) != Status::kStale ||
        fixture.channel.receiveWorkerFinalized(workerFinalized) != Status::kReceived ||
        workerFinalized.identity != fixture.grant.identity ||
        fixture.channel.receiveWorkerFinalized(duplicate_worker) != Status::kEmpty ||
        fixture.channel.acknowledgeWorkerFinalized(fixture.grant.identity) != Status::kPublished ||
        fixture.channel.acknowledgeWorkerFinalized(fixture.grant.identity) != Status::kStale) {
        throw std::runtime_error("worker-finalization acknowledgement failed");
    }

    Channel::TransportIntent intent;
    if (fixture.channel.receiveIntent(intent) != Status::kWrongState ||
        fixture.channel.acknowledgeIntentAfterHandoff(fixture.grant.identity, intent.token) !=
            Status::kWrongState) {
        throw std::runtime_error("intent publication survived finalization");
    }

    Channel::NetworkFinalized networkFinalized;
    Status received{};
    Status acknowledged{};
    Status postFinalIntent{};
    Status postFinalAckReceive{};
    fixture.worker.call([&](Scheduler&, Runtime&) {
        if (fixture.channel.acknowledgeNetworkFinalized(fixture.grant.identity) != Status::kStale) {
            throw std::runtime_error("network finalization acknowledged before consumption");
        }
        received = fixture.channel.receiveNetworkFinalized(networkFinalized);
        Channel::NetworkFinalized duplicate_network;
        if (fixture.channel.receiveNetworkFinalized(duplicate_network) != Status::kEmpty) {
            throw std::runtime_error("network finalization consumed more than once");
        }
        postFinalIntent = fixture.channel.publishIntent(fixture.grant.identity,
            resetIntent(fixture.grant.identity, 0, 1,
                ruvia::Http3ConnectionErrorCode::kRequestCancelled));
        Channel::TransportIntentAck lateAck;
        postFinalAckReceive = fixture.channel.receiveIntentAck(lateAck);
    });
    if (received != Status::kReceived || networkFinalized.identity != fixture.grant.identity ||
        postFinalIntent != Status::kWrongState || postFinalAckReceive != Status::kWrongState) {
        throw std::runtime_error("network finalization handoff/gate failed");
    }

    const auto networkWakeBeforeBorrow =
        fixture.networkWake.calls.load(std::memory_order_relaxed);
    const auto workerWakeBeforeBorrow =
        fixture.workerWake.calls.load(std::memory_order_relaxed);
    fixture.networkWake.blockNext.store(true, std::memory_order_release);
    std::thread acknowledgementThread([&] {
        fixture.worker.call([&](Scheduler&, Runtime&) {
            acknowledged = fixture.channel.acknowledgeNetworkFinalized(fixture.grant.identity);
        });
    });
    fixture.networkWake.entered.acquire();
    const bool blocked = fixture.channel.notificationBorrows() != 0 &&
                         !fixture.channel.readyToRearm();
    fixture.networkWake.release.release();
    acknowledgementThread.join();
    fixture.worker.call([&](Scheduler&, Runtime&) {
        if (fixture.channel.acknowledgeNetworkFinalized(fixture.grant.identity) != Status::kStale) {
            throw std::runtime_error("network finalization acknowledged more than once");
        }
    });
    if (!blocked || acknowledged != Status::kPublished ||
        fixture.networkWake.calls.load(std::memory_order_relaxed) < networkWakeBeforeBorrow + 2 ||
        fixture.workerWake.calls.load(std::memory_order_relaxed) <= workerWakeBeforeBorrow) {
        throw std::runtime_error("notification borrow did not fence and retry finalization");
    }
}

}  // namespace

RUVIA_TEST(http3ServerConnectionChannelPublishesCloseDuringAttachAckWindow) {
    CountingResource memory;
    {
        ChannelFixture fixture(memory);
        fixture.publishGrant(41, 9);

        fixture.transport.queueAccept();
        fixture.acceptAndCommit();
        const auto networkNotificationsBeforeAttach = fixture.networkWake.calls.load(
            std::memory_order_relaxed);
        fixture.bindAndAttach(true);
        RUVIA_CHECK(fixture.networkWake.calls.load(std::memory_order_relaxed) >=
                    networkNotificationsBeforeAttach + 2U);

        Channel::AttachResult attachResult{std::in_place_type<Channel::AttachAck>};
        RUVIA_CHECK(fixture.channel.receiveAttachResult(attachResult) == Status::kReceived);
        RUVIA_CHECK(std::holds_alternative<Channel::AttachAck>(attachResult));
        RUVIA_CHECK(std::get<Channel::AttachAck>(attachResult).identity == fixture.grant.identity);
        fixture.finishAttached(true);
        acknowledgeFinalization(fixture);
        RUVIA_CHECK(fixture.channel.readyToDestroy());
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
    RUVIA_CHECK_EQ(memory.allocations, memory.deallocations);
}

RUVIA_TEST(http3ServerConnectionChannelRetainsGrantUntilAcceptedAndRequiresRealAttach) {
    CountingResource memory;
    {
        ChannelFixture fixture(memory);
        fixture.publishGrant(41, 9);

        Channel::Identity firstPeek;
        Channel::Identity secondPeek;
        RUVIA_CHECK(fixture.channel.peekGrant(firstPeek) == Status::kReceived);
        RUVIA_CHECK(fixture.channel.peekGrant(secondPeek) == Status::kReceived);
        RUVIA_CHECK(firstPeek == fixture.grant.identity);
        RUVIA_CHECK(secondPeek == fixture.grant.identity);
        RUVIA_CHECK(!fixture.transport.acceptConnections(1));
        Channel::Identity stillPending;
        RUVIA_CHECK(fixture.channel.peekGrant(stillPending) == Status::kReceived);
        RUVIA_CHECK(stillPending == fixture.grant.identity);

        fixture.transport.queueAccept();
        fixture.acceptAndCommit();
        fixture.bindAndAttach();

        Channel::AttachResult attachResult{std::in_place_type<Channel::AttachAck>};
        RUVIA_CHECK(fixture.channel.receiveAttachResult(attachResult) == Status::kReceived);
        RUVIA_CHECK(std::holds_alternative<Channel::AttachAck>(attachResult));
        RUVIA_CHECK(std::get<Channel::AttachAck>(attachResult).identity == fixture.grant.identity);
        RUVIA_CHECK(fixture.channel.publishAdmissionSealed(
                        fixture.grant.identity, 2, 8) == Status::kPublished);
        Channel::AdmissionSealed sealed;
        Status sealStatus{};
        Status drainPublished{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            sealStatus = fixture.channel.receiveAdmissionSealed(sealed);
            drainPublished = fixture.channel.publishDrainComplete(fixture.grant.identity);
        });
        RUVIA_CHECK(sealStatus == Status::kReceived);
        RUVIA_CHECK(sealed.identity == fixture.grant.identity);
        RUVIA_CHECK_EQ(sealed.expectedAdmittedRequests, std::size_t{2});
        RUVIA_CHECK_EQ(sealed.goawayId, std::uint64_t{8});
        RUVIA_CHECK(drainPublished == Status::kPublished);
        Channel::DrainComplete drainComplete;
        RUVIA_CHECK(fixture.channel.receiveDrainComplete(drainComplete) == Status::kReceived);
        RUVIA_CHECK(drainComplete.identity == fixture.grant.identity);
        std::size_t attachedCount{};
        fixture.worker.call([&](Scheduler& scheduler, Runtime&) {
            attachedCount = scheduler.snapshot().attachedConnections;
        });
        RUVIA_CHECK_EQ(attachedCount, std::size_t{1});

        fixture.finishAttached();
        acknowledgeFinalization(fixture);
        RUVIA_CHECK(fixture.channel.readyToRearm());
        RUVIA_CHECK(fixture.channel.rearm() == Status::kPublished);

        const auto oldIdentity = fixture.grant.identity;
        RUVIA_CHECK(fixture.channel.closeNetworkPublications(oldIdentity) == Status::kWrongState);
        fixture.publishGrant(42, 1);
        RUVIA_CHECK(fixture.channel.publishTransportRetired(oldIdentity) == Status::kStale);
        Status newGenerationRetirement{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            Channel::TransportRetired retired;
            newGenerationRetirement = fixture.channel.receiveTransportRetired(retired);
        });
        RUVIA_CHECK(newGenerationRetirement == Status::kEmpty);
        RUVIA_CHECK(fixture.channel.closeNetworkPublications(oldIdentity) == Status::kWrongState);
        RUVIA_CHECK(fixture.grant.identity.epoch > oldIdentity.epoch);
        RUVIA_CHECK(fixture.grant.identity.slotGeneration > oldIdentity.slotGeneration);
        RUVIA_CHECK(fixture.channel.commitAccepted(oldIdentity) == Status::kStale);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(oldIdentity,
                        resetIntent(oldIdentity, 4, 1,
                            ruvia::Http3ConnectionErrorCode::kRequestCancelled)
                            .token) ==
                    Status::kStale);

        fixture.transport.queueAccept();
        fixture.acceptAndCommit();
        Channel::Bind bind;
        Status bindStatus{};
        Status rejectStatus{};
        fixture.worker.call([&](Scheduler& scheduler, Runtime&) {
            bindStatus = fixture.channel.receiveBind(bind);
            rejectStatus = fixture.channel.reject(scheduler, fixture.grant.registration,
                Channel::RejectReason::kConstructionFailed);
            if (fixture.channel.reject(scheduler, fixture.grant.registration,
                    Channel::RejectReason::kCapacity) != Status::kWrongState) {
                throw std::runtime_error("HTTP/3 rejection outcome changed before network handoff");
            }
        });
        RUVIA_CHECK(bindStatus == Status::kReceived);
        RUVIA_CHECK(bind.identity == fixture.grant.identity);
        RUVIA_CHECK(rejectStatus == Status::kPublished);
        Status rejectedIntentStatus{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            Connection::TransportIntent rejectedClose;
            rejectedClose.token = {.kind = Connection::TransportIntentKind::kConnectionClose,
                .id = {.epoch = fixture.grant.identity.epoch,
                    .connectionGeneration = fixture.grant.identity.connectionGeneration,
                    .streamId = 0},
                .sequence = std::numeric_limits<std::uint64_t>::max()};
            rejectedIntentStatus = fixture.channel.publishIntent(
                fixture.grant.identity, rejectedClose);
        });
        RUVIA_CHECK(rejectedIntentStatus == Status::kWrongState);
        RUVIA_CHECK(fixture.channel.receiveAttachResult(attachResult) == Status::kReceived);
        RUVIA_CHECK(std::holds_alternative<Channel::AttachRejected>(attachResult));
        RUVIA_CHECK(std::get<Channel::AttachRejected>(attachResult).reason ==
                    Channel::RejectReason::kConstructionFailed);
        RUVIA_CHECK(std::get<Channel::AttachRejected>(attachResult).identity ==
                    fixture.grant.identity);

        finishRejected(fixture, *fixture.acceptedId);
        acknowledgeFinalization(fixture);
        RUVIA_CHECK(fixture.channel.readyToDestroy());
        RUVIA_CHECK(fixture.channel.rearm() == Status::kPublished);
        Status stopAdmission{};
        Status idleWorkerClose{};
        Channel::GrantPublication stoppedGrant;
        fixture.worker.call([&](Scheduler& scheduler, Runtime&) {
            stopAdmission = fixture.channel.stopGrantPublication();
            idleWorkerClose = fixture.channel.closeWorkerPublications(fixture.grant.identity);
            stoppedGrant = fixture.channel.reserveAndPublishGrant(scheduler, 43, 1);
        });
        RUVIA_CHECK(stopAdmission == Status::kPublished);
        RUVIA_CHECK(idleWorkerClose == Status::kPublished);
        RUVIA_CHECK(stoppedGrant.status == Status::kWrongState);
        RUVIA_CHECK(fixture.channel.closeNetworkPublications(fixture.grant.identity) ==
                    Status::kPublished);
        RUVIA_CHECK(fixture.channel.readyToDestroy());
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
    RUVIA_CHECK_EQ(memory.allocations, memory.deallocations);
}

RUVIA_TEST(http3_server_connection_channel_consumes_late_drain_after_retirement) {
    CountingResource memory;
    {
        ChannelFixture fixture(memory);
        fixture.publishGrant(71, 16);
        fixture.transport.queueAccept();
        fixture.acceptAndCommit();
        fixture.bindAndAttach();
        Channel::AttachResult attached{std::in_place_type<Channel::AttachAck>};
        RUVIA_CHECK(fixture.channel.receiveAttachResult(attached) == Status::kReceived);
        RUVIA_CHECK(fixture.channel.publishAdmissionSealed(
                        fixture.grant.identity, 0, 0) == Status::kPublished);
        fixture.worker.call([&](Scheduler&, Runtime&) {
            Channel::AdmissionSealed sealed;
            RUVIA_CHECK(fixture.channel.receiveAdmissionSealed(sealed) == Status::kReceived);
            RUVIA_CHECK(sealed.identity == fixture.grant.identity);
            RUVIA_CHECK(fixture.channel.publishDrainComplete(fixture.grant.identity) ==
                        Status::kPublished);
        });

        Connection::TransportIntent close;
        fixture.worker.call([&](Scheduler& scheduler, Runtime& runtime) {
            RUVIA_CHECK(runtime.connection->requestStop());
            const auto step = scheduler.step();
            RUVIA_CHECK(step.kind == Scheduler::StepKind::kTransportIntent);
            RUVIA_CHECK(step.intent.token.kind == Connection::TransportIntentKind::kConnectionClose);
            close = step.intent;
            RUVIA_CHECK(fixture.channel.publishIntent(fixture.grant.identity, close) ==
                        Status::kPublished);
            RUVIA_CHECK(scheduler.beginRetirement(fixture.grant.registration.token));
        });
        fixture.retireTransport();

        Channel::DrainComplete drain;
        RUVIA_CHECK(fixture.channel.receiveDrainComplete(drain) == Status::kReceived);
        RUVIA_CHECK(drain.identity == fixture.grant.identity);
        Connection::TransportIntent networkClose;
        RUVIA_CHECK(fixture.channel.receiveIntent(networkClose) == Status::kReceived);
        RUVIA_CHECK(networkClose.token == close.token);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(
                        fixture.grant.identity, close.token,
                        Channel::IntentSettlement::kTransportRetiredSuperseded) ==
                    Status::kPublished);

        fixture.worker.callTask([&](Scheduler& scheduler, Runtime& runtime) -> ruvia::Task<void> {
            Channel::TransportRetired retired;
            RUVIA_CHECK(fixture.channel.receiveTransportRetired(retired) == Status::kReceived);
            RUVIA_CHECK(retired.identity == fixture.grant.identity);
            RUVIA_CHECK(runtime.connection->confirmTransportRetired({.epoch = fixture.grant.identity.epoch,
                .connectionGeneration = fixture.grant.identity.connectionGeneration}));
            Channel::TransportIntentAck closeAck;
            RUVIA_CHECK(fixture.channel.receiveIntentAck(closeAck) == Status::kReceived);
            RUVIA_CHECK(closeAck.token == close.token);
            RUVIA_CHECK(scheduler.acknowledgeIntent(
                fixture.grant.registration.token, closeAck.token));
            co_await runtime.connection->join();
            RUVIA_CHECK(scheduler.retire(fixture.grant.registration.token));
            runtime.connection.reset();
            RUVIA_CHECK(fixture.channel.closeWorkerPublications(fixture.grant.identity) ==
                        Status::kPublished);
        });
        RUVIA_CHECK(fixture.channel.closeNetworkPublications(fixture.grant.identity) ==
                    Status::kPublished);
        fixture.worker.call([&](Scheduler&, Runtime&) {
            RUVIA_CHECK(fixture.channel.publishWorkerFinalized(fixture.grant.identity) == Status::kPublished);
        });
        Channel::WorkerFinalized workerFinalized;
        RUVIA_CHECK(fixture.channel.receiveWorkerFinalized(workerFinalized) == Status::kReceived);
        RUVIA_CHECK(fixture.channel.acknowledgeWorkerFinalized(fixture.grant.identity) ==
                    Status::kPublished);
        RUVIA_CHECK(fixture.channel.publishNetworkFinalized(fixture.grant.identity) ==
                    Status::kPublished);
        fixture.worker.call([&](Scheduler&, Runtime&) {
            Channel::NetworkFinalized networkFinalized;
            RUVIA_CHECK(fixture.channel.receiveNetworkFinalized(networkFinalized) ==
                        Status::kReceived);
            RUVIA_CHECK(fixture.channel.acknowledgeNetworkFinalized(fixture.grant.identity) ==
                        Status::kPublished);
        });
        RUVIA_CHECK(fixture.channel.readyToDestroy());
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
    RUVIA_CHECK_EQ(memory.allocations, memory.deallocations);
}

RUVIA_TEST(http3ServerConnectionChannelSettlesRetiredIntentWithExactDispositionAndRetry) {
    CountingResource memory;
    {
        ChannelFixture fixture(memory);
        fixture.publishGrant(72, 17);
        fixture.transport.queueAccept();
        fixture.acceptAndCommit();
        fixture.bindAndAttach();
        Channel::AttachResult attached{std::in_place_type<Channel::AttachAck>};
        RUVIA_CHECK(fixture.channel.receiveAttachResult(attached) == Status::kReceived);
        RUVIA_CHECK(std::holds_alternative<Channel::AttachAck>(attached));

        std::array<Channel::TransportIntent, 3> resets{
            resetIntent(fixture.grant.identity, 4, 11,
                ruvia::Http3ConnectionErrorCode::kRequestCancelled),
            resetIntent(fixture.grant.identity, 8, 22,
                ruvia::Http3ConnectionErrorCode::kRequestCancelled),
            resetIntent(fixture.grant.identity, 12, 33,
                ruvia::Http3ConnectionErrorCode::kRequestCancelled),
        };
        std::array<Status, 2> published{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            published[0] = fixture.channel.publishIntent(fixture.grant.identity, resets[0]);
            published[1] = fixture.channel.publishIntent(fixture.grant.identity, resets[1]);
        });
        RUVIA_CHECK(published[0] == Status::kPublished);
        RUVIA_CHECK(published[1] == Status::kPublished);
        Channel::TransportIntent taken[2];
        RUVIA_CHECK(fixture.channel.receiveIntent(taken[0]) == Status::kReceived);
        RUVIA_CHECK(fixture.channel.receiveIntent(taken[1]) == Status::kReceived);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(
                        fixture.grant.identity, resets[0].token,
                        Channel::IntentSettlement::kExecutedHandoff) == Status::kPublished);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(
                        fixture.grant.identity, resets[1].token,
                        Channel::IntentSettlement::kExecutedHandoff) == Status::kPublished);

        Status lastPublished{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            lastPublished = fixture.channel.publishIntent(fixture.grant.identity, resets[2]);
        });
        RUVIA_CHECK(lastPublished == Status::kPublished);
        fixture.retireTransport();
        Channel::TransportIntent late;
        RUVIA_CHECK(fixture.channel.receiveIntent(late) == Status::kReceived);
        RUVIA_CHECK(late.token == resets[2].token);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(
                        fixture.grant.identity, late.token,
                        Channel::IntentSettlement::kTransportRetiredSuperseded) == Status::kFull);

        Status retirementReceived{};
        std::array<Status, 2> earlyAckStatuses{};
        std::array<Channel::TransportIntentAck, 2> earlyAcks{};
        fixture.worker.callTask([&](Scheduler&, Runtime& runtime) -> ruvia::Task<void> {
            Channel::TransportRetired retired;
            retirementReceived = fixture.channel.receiveTransportRetired(retired);
            if (retirementReceived != Status::kReceived || retired.identity != fixture.grant.identity ||
                !runtime.connection->confirmTransportRetired({.epoch = fixture.grant.identity.epoch,
                    .connectionGeneration = fixture.grant.identity.connectionGeneration})) {
                throw std::runtime_error("retired channel did not confirm its owner");
            }
            co_return;
        });
        RUVIA_CHECK(retirementReceived == Status::kReceived);

        fixture.worker.call([&](Scheduler&, Runtime&) {
            earlyAckStatuses[0] = fixture.channel.receiveIntentAck(earlyAcks[0]);
            earlyAckStatuses[1] = fixture.channel.receiveIntentAck(earlyAcks[1]);
        });
        RUVIA_CHECK(earlyAckStatuses[0] == Status::kReceived);
        RUVIA_CHECK(earlyAckStatuses[1] == Status::kReceived);
        RUVIA_CHECK(earlyAcks[0].settlement == Channel::IntentSettlement::kExecutedHandoff);
        RUVIA_CHECK(earlyAcks[1].settlement == Channel::IntentSettlement::kExecutedHandoff);

        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(
                        fixture.grant.identity, late.token,
                        Channel::IntentSettlement::kTransportRetiredSuperseded) == Status::kPublished);
        Channel::TransportIntentAck lateAck;
        Status lateAckStatus{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            lateAckStatus = fixture.channel.receiveIntentAck(lateAck);
        });
        RUVIA_CHECK(lateAckStatus == Status::kReceived);
        RUVIA_CHECK(lateAck.identity == fixture.grant.identity);
        RUVIA_CHECK(lateAck.token == late.token);
        RUVIA_CHECK(lateAck.settlement ==
                    Channel::IntentSettlement::kTransportRetiredSuperseded);

        fixture.worker.callTask([&](Scheduler& scheduler, Runtime& runtime) -> ruvia::Task<void> {
            if (!scheduler.beginRetirement(fixture.grant.registration.token)) {
                throw std::runtime_error("retired scheduler slot did not enter retirement");
            }
            co_await runtime.connection->join();
            if (!scheduler.retire(fixture.grant.registration.token)) {
                throw std::runtime_error("retired scheduler slot kept an intent debt");
            }
            runtime.connection.reset();
        });
        Status workerClosed{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            workerClosed = fixture.channel.closeWorkerPublications(fixture.grant.identity);
        });
        RUVIA_CHECK(workerClosed == Status::kPublished);
        RUVIA_CHECK(fixture.channel.closeNetworkPublications(fixture.grant.identity) ==
                    Status::kPublished);
        Channel::WorkerFinalized workerFinalized;
        fixture.worker.call([&](Scheduler&, Runtime&) {
            RUVIA_CHECK(fixture.channel.publishWorkerFinalized(fixture.grant.identity) ==
                        Status::kPublished);
        });
        RUVIA_CHECK(fixture.channel.receiveWorkerFinalized(workerFinalized) == Status::kReceived);
        RUVIA_CHECK(workerFinalized.identity == fixture.grant.identity);
        RUVIA_CHECK(fixture.channel.acknowledgeWorkerFinalized(fixture.grant.identity) ==
                    Status::kPublished);
        RUVIA_CHECK(fixture.channel.publishNetworkFinalized(fixture.grant.identity) ==
                    Status::kPublished);
        Channel::NetworkFinalized networkFinalized;
        fixture.worker.call([&](Scheduler&, Runtime&) {
            RUVIA_CHECK(fixture.channel.receiveNetworkFinalized(networkFinalized) ==
                        Status::kReceived);
            RUVIA_CHECK(fixture.channel.acknowledgeNetworkFinalized(fixture.grant.identity) ==
                        Status::kPublished);
        });
        RUVIA_CHECK(fixture.channel.readyToDestroy());
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
    RUVIA_CHECK_EQ(memory.allocations, memory.deallocations);
}

RUVIA_TEST(http3ServerConnectionChannelPreservesResetAndExactAckAcrossControlBackpressure) {
    CountingResource memory;
    {
        ChannelFixture fixture(memory);
        fixture.publishGrant(73, 18);
        fixture.transport.queueAccept();
        fixture.acceptAndCommit();
        fixture.bindAndAttach();
        Channel::AttachResult attachResult{std::in_place_type<Channel::AttachAck>};
        RUVIA_CHECK(fixture.channel.receiveAttachResult(attachResult) == Status::kReceived);
        RUVIA_CHECK(std::holds_alternative<Channel::AttachAck>(attachResult));

        std::array<Channel::TransportIntent, 3> resets{
            resetIntent(fixture.grant.identity, 4, 101, ruvia::Http3ConnectionErrorCode::kMessageError),
            resetIntent(fixture.grant.identity, 8, 202, ruvia::Http3ConnectionErrorCode::kRequestCancelled),
            resetIntent(fixture.grant.identity, 12, 303, ruvia::Http3ConnectionErrorCode::kExcessiveLoad),
        };
        std::array<Status, 3> publishStatuses{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            publishStatuses[0] = fixture.channel.publishIntent(fixture.grant.identity, resets[0]);
            publishStatuses[1] = fixture.channel.publishIntent(fixture.grant.identity, resets[1]);
            publishStatuses[2] = fixture.channel.publishIntent(fixture.grant.identity, resets[2]);
        });
        RUVIA_CHECK(publishStatuses[0] == Status::kPublished);
        RUVIA_CHECK(publishStatuses[1] == Status::kPublished);
        RUVIA_CHECK(publishStatuses[2] == Status::kFull);

        auto forged = resets[0].token;
        --forged.sequence;
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(
                        fixture.grant.identity, forged) == Status::kStale);

        const auto workerWakeBeforeDequeue = fixture.workerWake.calls.load(std::memory_order_relaxed);
        Channel::TransportIntent received;
        RUVIA_CHECK(fixture.channel.receiveIntent(received) == Status::kReceived);
        RUVIA_CHECK(received.token == resets[0].token);
        RUVIA_CHECK(received.streamResetErrorCode == resets[0].streamResetErrorCode);
        RUVIA_CHECK(received.closeReason == resets[0].closeReason);
        RUVIA_CHECK(received.connectionErrorCode == resets[0].connectionErrorCode);
        RUVIA_CHECK(fixture.workerWake.calls.load(std::memory_order_relaxed) >
                    workerWakeBeforeDequeue);

        Status retryStatus{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            retryStatus = fixture.channel.publishIntent(fixture.grant.identity, resets[2]);
        });
        RUVIA_CHECK(retryStatus == Status::kPublished);
        Channel::TransportIntent secondReceived;
        Channel::TransportIntent thirdReceived;
        RUVIA_CHECK(fixture.channel.receiveIntent(secondReceived) == Status::kReceived);
        RUVIA_CHECK(fixture.channel.receiveIntent(thirdReceived) == Status::kReceived);
        RUVIA_CHECK(secondReceived.token == resets[1].token);
        RUVIA_CHECK(thirdReceived.token == resets[2].token);

        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(
                        fixture.grant.identity, resets[0].token) == Status::kPublished);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(
                        fixture.grant.identity, resets[1].token) == Status::kPublished);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(
                        fixture.grant.identity, resets[2].token) == Status::kFull);

        Channel::TransportIntentAck ack;
        std::array<Status, 2> receiveAckStatuses{};
        std::array<Channel::TransportIntentAck, 2> receivedAcks{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            receiveAckStatuses[0] = fixture.channel.receiveIntentAck(receivedAcks[0]);
            receiveAckStatuses[1] = fixture.channel.receiveIntentAck(receivedAcks[1]);
        });
        RUVIA_CHECK(receiveAckStatuses[0] == Status::kReceived);
        RUVIA_CHECK(receiveAckStatuses[1] == Status::kReceived);
        RUVIA_CHECK(receivedAcks[0].identity == fixture.grant.identity);
        RUVIA_CHECK(receivedAcks[0].token == resets[0].token);
        RUVIA_CHECK(receivedAcks[1].identity == fixture.grant.identity);
        RUVIA_CHECK(receivedAcks[1].token == resets[1].token);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(
                        fixture.grant.identity, resets[2].token) == Status::kPublished);
        Status finalAckStatus{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            finalAckStatus = fixture.channel.receiveIntentAck(ack);
        });
        RUVIA_CHECK(finalAckStatus == Status::kReceived);
        RUVIA_CHECK(ack.token == resets[2].token);

        std::array<Status, 2> refillStatuses{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            refillStatuses[0] = fixture.channel.publishIntent(fixture.grant.identity,
                resetIntent(fixture.grant.identity, 16, 404,
                    ruvia::Http3ConnectionErrorCode::kMessageError));
            refillStatuses[1] = fixture.channel.publishIntent(fixture.grant.identity,
                resetIntent(fixture.grant.identity, 20, 505,
                    ruvia::Http3ConnectionErrorCode::kRequestCancelled));
        });
        RUVIA_CHECK(refillStatuses[0] == Status::kPublished);
        RUVIA_CHECK(refillStatuses[1] == Status::kPublished);

        fixture.finishAttached();
        // The close record is independent of the full CONTROL lane and supersedes
        // queued stream resets; finishAttached() also proves exact close-token ACK.
        RUVIA_CHECK(fixture.channel.readyToDestroy() == false);
        acknowledgeFinalization(fixture);
        RUVIA_CHECK(fixture.channel.readyToDestroy());
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
    RUVIA_CHECK_EQ(memory.allocations, memory.deallocations);
}

RUVIA_TEST(http3ServerConnectionChannelClosesPublicationGatesAndRejectsOldGeneration) {
    CountingResource memory;
    {
        ChannelFixture fixture(memory);
        fixture.publishGrant(90, 1);
        RUVIA_CHECK(fixture.channel.revokeGrant() == Status::kPublished);

        Channel::Identity revokeIdentity;
        Status receiveStatus{};
        Status ackStatus{};
        fixture.worker.call([&](Scheduler& scheduler, Runtime&) {
            receiveStatus = fixture.channel.receiveRevoke(revokeIdentity);
            ackStatus = fixture.channel.acknowledgeRevoke(scheduler, fixture.grant.registration);
        });
        RUVIA_CHECK(receiveStatus == Status::kReceived);
        RUVIA_CHECK(revokeIdentity == fixture.grant.identity);
        RUVIA_CHECK(ackStatus == Status::kPublished);
        Channel::RevokeAck revokeAck;
        RUVIA_CHECK(fixture.channel.receiveRevokeAck(revokeAck) == Status::kReceived);
        RUVIA_CHECK(revokeAck.identity == fixture.grant.identity);

        Status workerClosed{};
        Status noRevokedRetirement{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            Channel::TransportRetired retired;
            noRevokedRetirement = fixture.channel.receiveTransportRetired(retired);
            workerClosed = fixture.channel.closeWorkerPublications(fixture.grant.identity);
        });
        RUVIA_CHECK(noRevokedRetirement == Status::kEmpty);
        RUVIA_CHECK(workerClosed == Status::kPublished);
        RUVIA_CHECK(fixture.channel.closeNetworkPublications(fixture.grant.identity) ==
                    Status::kPublished);
        RUVIA_CHECK(fixture.channel.readyToRearm());

        const auto oldIdentity = fixture.grant.identity;
        RUVIA_CHECK(fixture.channel.rearm() == Status::kPublished);
        fixture.publishGrant(91, 1);
        RUVIA_CHECK(fixture.grant.identity.epoch > oldIdentity.epoch);
        RUVIA_CHECK(fixture.grant.identity.slotGeneration > oldIdentity.slotGeneration);
        RUVIA_CHECK(fixture.channel.commitAccepted(oldIdentity) == Status::kStale);
        Status admissionSealed{};
        Status staleClose{};
        Channel::GrantPublication blockedGrant;
        fixture.worker.call([&](Scheduler& scheduler, Runtime&) {
            admissionSealed = fixture.channel.stopGrantPublication();
            Connection::TransportIntent oldClose;
            oldClose.token = {.kind = Connection::TransportIntentKind::kConnectionClose,
                .id = {.epoch = oldIdentity.epoch,
                    .connectionGeneration = oldIdentity.connectionGeneration,
                    .streamId = 0},
                .sequence = std::numeric_limits<std::uint64_t>::max()};
            staleClose = fixture.channel.publishIntent(oldIdentity, oldClose);
            blockedGrant = fixture.channel.reserveAndPublishGrant(scheduler, 92, 1);
        });
        RUVIA_CHECK(admissionSealed == Status::kPublished);
        RUVIA_CHECK(staleClose == Status::kStale);
        RUVIA_CHECK(blockedGrant.status == Status::kWrongState);

        RUVIA_CHECK(fixture.channel.revokeGrant() == Status::kPublished);
        fixture.worker.call([&](Scheduler& scheduler, Runtime&) {
            if (fixture.channel.receiveRevoke(revokeIdentity) != Status::kReceived ||
                fixture.channel.acknowledgeRevoke(scheduler, fixture.grant.registration) !=
                    Status::kPublished) {
                throw std::runtime_error("second generation revoke failed");
            }
        });
        RUVIA_CHECK(fixture.channel.receiveRevokeAck(revokeAck) == Status::kReceived);

        workerClosed = Status::kWrongState;
        fixture.worker.call([&](Scheduler&, Runtime&) {
            workerClosed = fixture.channel.closeWorkerPublications(fixture.grant.identity);
            if (workerClosed == Status::kPublished &&
                fixture.channel.publishIntent(fixture.grant.identity,
                    resetIntent(fixture.grant.identity, 0, 1,
                        ruvia::Http3ConnectionErrorCode::kRequestCancelled)) != Status::kWrongState) {
                throw std::runtime_error("worker intent publication survived its close gate");
            }
        });
        RUVIA_CHECK(workerClosed == Status::kPublished);
        RUVIA_CHECK(fixture.channel.closeNetworkPublications(fixture.grant.identity) ==
                    Status::kPublished);
        RUVIA_CHECK(fixture.channel.readyToDestroy());
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
    RUVIA_CHECK_EQ(memory.allocations, memory.deallocations);
}

RUVIA_TEST(http3ServerConnectionChannelCarriesPushOpenResultsAcrossBothOwnerThreadsAndAckBackpressure) {
    CountingResource memory;
    {
        ChannelFixture fixture(memory);
        fixture.publishGrant(73, 18);
        fixture.transport.queueAccept();
        fixture.acceptAndCommit();
        fixture.bindAndAttach();
        Channel::AttachResult attached{std::in_place_type<Channel::AttachAck>};
        RUVIA_CHECK(fixture.channel.receiveAttachResult(attached) == Status::kReceived);
        std::array<Channel::TransportIntent, 3> intents{};
        for (std::size_t i = 0; i < intents.size(); ++i) {
            intents[i].token = {.kind = Connection::TransportIntentKind::kOpenPushStream,
                .id = {fixture.grant.identity.epoch, fixture.grant.identity.connectionGeneration, 0, i},
                .sequence = i + 1};
        }
        std::array<Status, 3> statuses{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            for (std::size_t i = 0; i < intents.size(); ++i) {
                statuses[i] = fixture.channel.publishIntent(fixture.grant.identity, intents[i]);
            }
        });
        RUVIA_CHECK(statuses[0] == Status::kPublished);
        RUVIA_CHECK(statuses[1] == Status::kPublished);
        RUVIA_CHECK(statuses[2] == Status::kFull);
        Channel::TransportIntent first;
        Channel::TransportIntent second;
        Channel::TransportIntent third;
        RUVIA_CHECK(fixture.channel.receiveIntent(first) == Status::kReceived);
        fixture.worker.call([&](Scheduler&, Runtime&) {
            statuses[2] = fixture.channel.publishIntent(fixture.grant.identity, intents[2]);
        });
        RUVIA_CHECK(statuses[2] == Status::kPublished);
        RUVIA_CHECK(fixture.channel.receiveIntent(second) == Status::kReceived);
        RUVIA_CHECK(fixture.channel.receiveIntent(third) == Status::kReceived);
        const Connection::PushStreamOpenResult opened{.status = Connection::PushStreamOpenResult::Status::kOpened, .streamId = 31};
        const Connection::PushStreamOpenResult unavailable{};
        const Connection::PushStreamOpenResult stopped{.status = Connection::PushStreamOpenResult::Status::kStopped};
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(fixture.grant.identity, first.token) == Status::kWrongState);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(fixture.grant.identity, first.token,
                        Channel::IntentSettlement::kExecutedHandoff, Connection::PushStreamOpenResult{.status = Connection::PushStreamOpenResult::Status::kOpened, .streamId = 0}) == Status::kWrongState);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(fixture.grant.identity, first.token,
                        Channel::IntentSettlement::kExecutedHandoff, Connection::PushStreamOpenResult{.status = Connection::PushStreamOpenResult::Status::kOpened, .streamId = std::numeric_limits<std::uint64_t>::max()}) == Status::kWrongState);
        auto forged = first.token;
        forged.id.pushId = 123;
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(fixture.grant.identity, forged,
                        Channel::IntentSettlement::kExecutedHandoff, opened) == Status::kStale);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(fixture.grant.identity, first.token,
                        Channel::IntentSettlement::kExecutedHandoff, opened) == Status::kPublished);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(fixture.grant.identity, second.token,
                        Channel::IntentSettlement::kExecutedHandoff, unavailable) == Status::kPublished);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(fixture.grant.identity, third.token,
                        Channel::IntentSettlement::kExecutedHandoff, stopped) == Status::kFull);
        std::array<Channel::TransportIntentAck, 3> acks{};
        fixture.worker.call([&](Scheduler&, Runtime&) {
            RUVIA_CHECK(fixture.channel.receiveIntentAck(acks[0]) == Status::kReceived);
            RUVIA_CHECK(fixture.channel.receiveIntentAck(acks[1]) == Status::kReceived);
        });
        RUVIA_CHECK_EQ(acks[0].pushStream->streamId, std::uint64_t{31});
        RUVIA_CHECK(acks[0].pushStream->status == Connection::PushStreamOpenResult::Status::kOpened);
        RUVIA_CHECK(acks[1].pushStream->status == Connection::PushStreamOpenResult::Status::kUnavailable);
        RUVIA_CHECK(fixture.channel.acknowledgeIntentAfterHandoff(fixture.grant.identity, third.token,
                        Channel::IntentSettlement::kExecutedHandoff, stopped) == Status::kPublished);
        fixture.worker.call([&](Scheduler&, Runtime&) {
            RUVIA_CHECK(fixture.channel.receiveIntentAck(acks[2]) == Status::kReceived);
        });
        RUVIA_CHECK(acks[2].pushStream->status == Connection::PushStreamOpenResult::Status::kStopped);
        RUVIA_CHECK_EQ(acks[2].token.id.pushId, std::optional<std::uint64_t>{2});
        fixture.finishAttached(false, 4);
        acknowledgeFinalization(fixture);
        RUVIA_CHECK(fixture.channel.readyToDestroy());
    }
    RUVIA_CHECK_EQ(memory.allocated, memory.freed);
    RUVIA_CHECK_EQ(memory.allocations, memory.deallocations);
}

RUVIA_TEST(http3_server_connection_channel_datagrams_preserve_boundaries_drop_on_capacity_and_retire_both_lanes) {
    CountingResource memory;
    {
        ChannelFixture fixture(memory, true);
        fixture.publishGrant(1, 1);
        fixture.transport.queueAccept();
        fixture.acceptAndCommit();
        fixture.bindAndAttach();
        Channel::AttachResult attached{std::in_place_type<Channel::AttachAck>};
        RUVIA_CHECK(fixture.channel.receiveAttachResult(attached) == Status::kReceived);
        const std::array<std::byte, 3> bytes{std::byte{1}, std::byte{2}, std::byte{3}};
        const auto allocations = memory.allocations;
        for (std::size_t i = 0; i < Channel::kDatagramCapacity; ++i) {
            RUVIA_CHECK(fixture.channel.publishRequestDatagram(fixture.grant.identity, 4, i == 0 ? std::span<const std::byte>{} : std::span(bytes)) == Status::kPublished);
        }
        RUVIA_CHECK(fixture.channel.publishRequestDatagram(fixture.grant.identity, 4, bytes) == Status::kFull);
        RUVIA_CHECK(memory.allocations == allocations);
        fixture.worker.call([&](Scheduler&, Runtime&) {
            Channel::Datagram message;
            RUVIA_CHECK(fixture.channel.receiveRequestDatagram(message) == Status::kReceived);
            RUVIA_CHECK(message.identity == fixture.grant.identity && message.streamId == 4 && message.size == 0);
            RUVIA_CHECK(fixture.channel.receiveRequestDatagram(message) == Status::kReceived);
            RUVIA_CHECK(message.size == bytes.size() && std::equal(bytes.begin(), bytes.end(), message.bytes.begin()));
            for (std::size_t i = 0; i < Channel::kDatagramCapacity; ++i) {
                RUVIA_CHECK(fixture.channel.publishResponseDatagram(fixture.grant.identity, 8, bytes) == Status::kPublished);
            }
            RUVIA_CHECK(fixture.channel.publishResponseDatagram(fixture.grant.identity, 8, bytes) == Status::kFull);
        });
        Channel::Datagram message;
        RUVIA_CHECK(fixture.channel.receiveResponseDatagram(message) == Status::kReceived);
        RUVIA_CHECK(message.identity == fixture.grant.identity && message.streamId == 8 && message.size == bytes.size());
        // Leave packets in both directions through transport retirement. Closed
        // publication gates precede each consumer's final discard and ACK.
        fixture.finishAttached();
        acknowledgeFinalization(fixture);
        RUVIA_CHECK(fixture.channel.receiveResponseDatagram(message) == Status::kEmpty);
        fixture.worker.call([&](Scheduler&, Runtime&) {
            RUVIA_CHECK(fixture.channel.receiveRequestDatagram(message) == Status::kEmpty);
            RUVIA_CHECK(fixture.channel.publishResponseDatagram(fixture.grant.identity, 8, bytes) == Status::kWrongState);
        });
        RUVIA_CHECK(fixture.channel.readyToDestroy());
    }
    RUVIA_CHECK(memory.allocated == memory.freed);
}
