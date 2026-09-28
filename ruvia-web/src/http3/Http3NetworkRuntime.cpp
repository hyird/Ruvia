#include "ruvia/web/detail/http3/Http3NetworkRuntime.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/http/Http3PeerStreams.h"
#include "ruvia/http/Http3StreamFrames.h"
#include "ruvia/web/detail/http3/Http3WorkerServer.h"
#include "ruvia/web/detail/server/HttpServerListener.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"

namespace ruvia::detail {
namespace {

constexpr auto kServerShutdownCode = Http3ConnectionErrorCode::kNoError;
constexpr auto kProtocolFailureCode = Http3ConnectionErrorCode::kInternalError;
constexpr auto kMonitorInterval = std::chrono::milliseconds(10);

bool phaseTimeoutExpired(std::optional<std::chrono::milliseconds> timeout,
    std::chrono::steady_clock::time_point lastActivity,
    std::chrono::steady_clock::time_point now) noexcept {
    return timeout && now >= lastActivity && now - lastActivity >= *timeout;
}

std::size_t totalConnectionCapacity(std::span<const Http3NetworkRuntime::WorkerTarget> workers) {
    if (workers.empty()) {
        throw std::invalid_argument("HTTP/3 server network requires at least one worker");
    }
    std::size_t total = 0;
    for (const auto& worker : workers) {
        if (worker.server == nullptr || worker.maxConnections == 0 ||
            worker.mailboxCapacity == 0 || worker.maxRequestsPerConnection == 0 ||
            worker.maxConnections > std::numeric_limits<std::size_t>::max() - total) {
            throw std::invalid_argument("invalid HTTP/3 server network worker target");
        }
        total += worker.maxConnections;
    }
    return total;
}

const HttpServerListenerDefinition::Tls& requireTls(
    const HttpServerListenerDefinition& listener) {
    const auto* tls = std::get_if<HttpServerListenerDefinition::Tls>(&listener.transport);
    if (tls == nullptr || !listener.http3.has_value()) {
        throw std::invalid_argument("HTTP/3 server network requires TLS and explicit configuration");
    }
    return *tls;
}

Http3QuicServerTransportConfig transportConfig(
    const HttpServerListenerDefinition& listener,
    std::span<const Http3NetworkRuntime::WorkerTarget> workers) {
    if (workers.empty()) {
        throw std::invalid_argument("HTTP/3 server network requires at least one worker");
    }
    const auto& first = workers.front();
    const auto idleTimeout = first.idleTimeout;
    if (std::ranges::any_of(workers,
            [&first](const Http3NetworkRuntime::WorkerTarget& worker) {
                return worker.idleTimeout != first.idleTimeout ||
                       worker.requestHeaderTimeout != first.requestHeaderTimeout ||
                       worker.requestBodyTimeout != first.requestBodyTimeout ||
                       worker.writeTimeout != first.writeTimeout ||
                       worker.maxRequestsPerConnection != first.maxRequestsPerConnection;
            })) {
        throw std::invalid_argument("HTTP/3 workers require normalized phase timeouts");
    }
    return {.maxActiveConnections = totalConnectionCapacity(workers),
        .maxLifetimePeerStreams =
            http3TransportLifetimeStreamCapacity(first.maxRequestsPerConnection),
        .handshakeTimeout = listener.http3->handshakeTimeout,
        .idleTimeout = idleTimeout};
}

asio::ip::udp::endpoint udpEndpoint(const HttpServerListenerDefinition& listener) {
    return {listener.endpoint.address(), listener.endpoint.port()};
}

}  // namespace

Http3NetworkRuntime::WorkerLink::WorkerLink(std::pmr::memory_resource* resource,
    Http3NetworkRuntime& network, WorkerTarget configured)
    : target(configured),
      requestMailbox(configured.mailboxCapacity, configured.mailboxCapacity,
          configured.mailboxCapacity, resource,
          Http3StreamMailboxCapacityNotifier{&network, &Http3NetworkRuntime::networkWake}),
      channels(resource),
      channelViews(resource),
      connections(resource) {
    channels.reserve(target.maxConnections);
    channelViews.reserve(target.maxConnections);
    connections.reserve(target.maxConnections);
    const Http3ServerConnectionChannel::Notification networkNotification{
        &network, &Http3NetworkRuntime::networkWake};
    const Http3ServerConnectionChannel::Notification workerNotification{
        target.server, &Http3NetworkRuntime::workerWake};
    for (std::size_t i = 0; i < target.maxConnections; ++i) {
        channels.push_back(makePmrObject<Http3ServerConnectionChannel>(
            resource, networkNotification, workerNotification));
        channelViews.push_back(channels.back().get());
        connections.emplace_back(resource);
    }
}

Http3NetworkRuntime::Http3NetworkRuntime(ruvia::WorkerRuntimeContext& networkRuntime,
    const HttpServerListenerDefinition& listener, std::span<const WorkerTarget> workers,
    FailureNotification failure)
    : networkRuntime_(networkRuntime),
      ioContext_(networkRuntime.ioContext()),
      ownerThread_(std::this_thread::get_id()),
      memory_(),
      bindAddress_(listener.endpoint.address()),
      tls_(requireTls(listener), memory_.resource()),
      wire_(ioContext_, udpEndpoint(listener), tls_, transportConfig(listener, workers),
          memory_.resource(), {this, [](void* context, Http3QuicServerTransport&) noexcept {
                                   auto& self = *static_cast<Http3NetworkRuntime*>(context);
                                   const bool progress = self.protocolPump();
                                   if (self.failure_) {
                                       return Http3QuicWireOwner::ProtocolPumpResult::kFatal;
                                   }
                                   return progress
                                              ? Http3QuicWireOwner::ProtocolPumpResult::kProgress
                                              : Http3QuicWireOwner::ProtocolPumpResult::kIdle;
                               }}),
      workers_(memory_.resource()),
      monitorTimer_(ioContext_),
      failureNotification_(failure),
      drainTimeout_(listener.http3->drainTimeout) {
    if (drainTimeout_ <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("HTTP/3 drain timeout must be greater than zero");
    }
    workers_.reserve(workers.size());
    for (const auto& worker : workers) {
        workers_.push_back(
            makePmrObject<WorkerLink>(memory_.resource(), memory_.resource(), *this, worker));
    }
    try {
        wire_.prepare();
    } catch (...) {
        wire_.requestStop();
        wire_.pollStop();
        if (!wire_.stopStatus().complete()) {
            std::terminate();
        }
        throw;
    }
}

Http3NetworkRuntime::~Http3NetworkRuntime() {
    requireOwnerThread();
    if (running_ || monitorScheduled_ || !drained()) {
        std::terminate();
    }
}

void Http3NetworkRuntime::stageWorkerLinks() {
    requireOwnerThread();
    if (staged_) {
        throw std::logic_error("HTTP/3 server network worker links already staged");
    }
    for (auto& owned : workers_) {
        auto& worker = *owned;
        if (!worker.target.server->stageInstall({
                .requestMailbox = &worker.requestMailbox,
                .channels = worker.channelViews,
                .networkWake = {this, &networkWake},
            })) {
            throw std::runtime_error("failed to stage HTTP/3 worker link");
        }
    }
    staged_ = true;
}

void Http3NetworkRuntime::start() {
    requireOwnerThread();
    if (!staged_ || running_ || stopping_) {
        throw std::logic_error("HTTP/3 server network cannot start in this state");
    }
    running_ = true;
    wire_.start();
    wire_.requestDrive();
    scheduleMonitor();
}

void Http3NetworkRuntime::wake() noexcept {
    if (wakeScheduled_.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    networkRuntime_.deferOrTerminate([this] {
        requireOwnerThread();
        wakeScheduled_.store(false, std::memory_order_release);
        if (stopping_) {
            (void)protocolPump();
            wire_.pollStop();
            scheduleMonitor();
        } else if (running_) {
            wire_.requestDrive();
        }
    });
}

void Http3NetworkRuntime::stop() noexcept {
    requireOwnerThread();
    const bool alreadyStopping = stopping_;
    requestStopOnOwner();
    if (!alreadyStopping) {
        (void)protocolPump();
    }
    wire_.pollStop();
    if (!drained()) {
        scheduleMonitor();
    }
}

bool Http3NetworkRuntime::drained() const noexcept {
    requireOwnerThread();
    if (!stopping_) {
        return false;
    }
    const bool channelsDone = std::ranges::all_of(workers_, [](const auto& owned) {
        return std::ranges::all_of(owned->channels,
            [](const auto& channel) { return channel->readyToDestroy(); });
    });
    const bool workersDone = std::ranges::all_of(workers_, [](const auto& owned) {
        return owned->target.server->drained();
    });
    return channelsDone && workersDone && wire_.stopStatus().complete();
}

asio::ip::udp::endpoint Http3NetworkRuntime::localEndpoint() const {
    requireOwnerThread();
    return {bindAddress_, wire_.boundPort()};
}

void Http3NetworkRuntime::networkWake(void* context) noexcept {
    static_cast<Http3NetworkRuntime*>(context)->wake();
}

void Http3NetworkRuntime::workerWake(void* context) noexcept {
    auto& worker = *static_cast<Http3WorkerServer*>(context);
    (void)worker.notification().notify();
}

void Http3NetworkRuntime::requireOwnerThread() const noexcept {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
}

void Http3NetworkRuntime::requestStopOnOwner() noexcept {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    for (auto& owned : workers_) {
        auto& worker = *owned;
        (void)worker.requestMailbox.stop();
        for (std::size_t i = 0; i < worker.connections.size(); ++i) {
            auto& connection = worker.connections[i];
            if (!connection.grantReceived) {
                continue;
            }
            if (connection.bindPublished) {
                if (connection.accepted) {
                    closeConnection(connection, kServerShutdownCode);
                }
            } else {
                (void)requestRevoke(worker, i);
                (void)retireUnbound(worker, i);
            }
        }
    }
}

void Http3NetworkRuntime::reportFailure(std::exception_ptr failure) noexcept {
    requireOwnerThread();
    if (failure_ == nullptr) {
        failure_ = failure;
    }
    requestStopOnOwner();
    if (failureReported_) {
        return;
    }
    failureReported_ = true;
    if (failureNotification_.notify != nullptr) {
        failureNotification_.notify(failureNotification_.context, failure_);
    }
    scheduleMonitor();
}

void Http3NetworkRuntime::scheduleMonitor() noexcept {
    requireOwnerThread();
    if (monitorScheduled_ || (!running_ && !stopping_) || drained()) {
        return;
    }
    monitorScheduled_ = true;
    try {
        monitorTimer_.expires_after(kMonitorInterval);
        monitorTimer_.async_wait([this](const asio::error_code& error) noexcept {
            monitor(error);
        });
    } catch (...) {
        monitorScheduled_ = false;
        reportFailure(std::current_exception());
        if (!drained()) {
            // A failed monitor initiation must not leave joined owners waiting
            // on a wire callback that can no longer make ordered progress.
            std::terminate();
        }
    }
}

void Http3NetworkRuntime::monitor(const asio::error_code& error) noexcept {
    requireOwnerThread();
    monitorScheduled_ = false;
    if (error) {
        if (error != asio::error::operation_aborted) {
            try {
                throw std::system_error(error, "HTTP/3 server network monitor timer");
            } catch (...) {
                reportFailure(std::current_exception());
            }
        }
    } else {
        if (const auto failure = wire_.failure()) {
            reportFailure(failure);
        }
        if (stopping_) {
            (void)protocolPump();
            wire_.pollStop();
        } else if (running_) {
            wire_.requestDrive();
        }
    }
    if (!drained()) {
        scheduleMonitor();
    }
}

bool Http3NetworkRuntime::protocolPump() noexcept {
    requireOwnerThread();
    if (!running_ && !stopping_) {
        return false;
    }

    try {
        bool anyProgress = false;
        bool exhaustedBudget = true;
        for (std::size_t pass = 0; pass < pumpBudget_; ++pass) {
            bool progress = false;
            if (!workers_.empty()) {
                for (std::size_t offset = 0; offset < workers_.size(); ++offset) {
                    const auto index = (nextWorker_ + offset) % workers_.size();
                    progress = pumpWorker(*workers_[index]) || progress;
                }
                nextWorker_ = (nextWorker_ + 1) % workers_.size();
            }
            if (!progress) {
                exhaustedBudget = false;
                break;
            }
            anyProgress = true;
        }

        if (stopping_) {
            const bool channelsDone = std::ranges::all_of(workers_, [](const auto& owned) {
                return std::ranges::all_of(owned->channels,
                    [](const auto& channel) { return channel->readyToDestroy(); });
            });
            if (channelsDone && !wire_.stopStatus().stopping) {
                wire_.requestStop();
                anyProgress = true;
            }
            wire_.pollStop();
            if (channelsDone && wire_.stopStatus().complete() &&
                std::ranges::all_of(workers_, [](const auto& owned) {
                    return owned->target.server->drained();
                })) {
                running_ = false;
            }
        } else if (exhaustedBudget) {
            // The QUIC wire owner bounds each turn. Queue a coalesced follow-up
            // so a long run of ready mailbox work cannot strand the connection
            // after the last UDP/timer completion.
            wake();
        }
        return anyProgress;
    } catch (...) {
        reportFailure(std::current_exception());
        return false;
    }
}

bool Http3NetworkRuntime::pumpWorker(WorkerLink& worker) noexcept {
    bool progress = pumpChannels(worker);
    for (std::size_t i = 0; i < worker.connections.size(); ++i) {
        auto& connection = worker.connections[i];
        if (connection.attached && !connection.closeStarted &&
            !connection.gracefulCloseStarted) {
            auto* transport = wire_.transport();
            const auto info = transport == nullptr
                                  ? std::optional<Http3QuicServerTransport::ConnectionInfo>{}
                                  : transport->connectionInfo(connection.transportId);
            if (!info || info->terminated) {
                // Peer CONNECTION_CLOSE and negotiated QUIC idle expiry are
                // terminal even when there is no request stream left to make a
                // read/write call observe them. Retire the worker binding and
                // return its shared connection credit.
                closeConnection(connection, kServerShutdownCode);
                progress = true;
            } else {
                progress = pumpInput(worker, i) || progress;
                progress = pumpOutput(worker, i) || progress;
            }
        }
    }
    progress = pumpResponses(worker) || progress;
    for (std::size_t i = 0; i < worker.connections.size(); ++i) {
        progress = retire(worker, i) || progress;
    }
    return progress;
}

bool Http3NetworkRuntime::pumpChannels(WorkerLink& worker) noexcept {
    bool progress = false;
    for (std::size_t i = 0; i < worker.channels.size(); ++i) {
        auto& channel = *worker.channels[i];
        auto& connection = worker.connections[i];
        if (!connection.grantReceived) {
            Http3ServerConnectionChannel::Identity identity;
            const auto status = channel.peekGrant(identity);
            if (status == Http3ServerConnectionChannel::Status::kReceived) {
                connection.identity = identity;
                connection.grantReceived = true;
                progress = true;
            } else if (status != Http3ServerConnectionChannel::Status::kEmpty) {
                std::terminate();
            }
        }
        if (connection.grantReceived && !connection.bindPublished &&
            !connection.revokeRequested) {
            progress = stopping_ ? (requestRevoke(worker, i) || progress)
                                 : (admit(worker, i) || progress);
        }
        if (connection.bindPublished && !connection.attachResolved) {
            progress = attach(worker, i) || progress;
        }

        if (connection.pendingIntentAck) {
            const auto acknowledged = channel.acknowledgeIntentAfterHandoff(
                connection.identity, *connection.pendingIntentAck);
            if (acknowledged == Http3ServerConnectionChannel::Status::kPublished) {
                connection.pendingIntentAck.reset();
                progress = true;
            } else if (acknowledged != Http3ServerConnectionChannel::Status::kFull) {
                closeConnection(connection, kProtocolFailureCode);
            }
        }
        if (connection.bindPublished && !connection.transportRetiredPublished &&
            !connection.pendingIntentAck) {
            Http3ServerConnectionChannel::TransportIntent intent;
            for (;;) {
                const auto status = channel.receiveIntent(intent);
                if (status == Http3ServerConnectionChannel::Status::kEmpty) {
                    break;
                }
                if (status != Http3ServerConnectionChannel::Status::kReceived ||
                    intent.token.id.epoch != connection.identity.epoch ||
                    intent.token.id.connectionGeneration !=
                        connection.identity.connectionGeneration) {
                    std::terminate();
                }
                progress = true;
                if (intent.token.kind ==
                    Http3BufferedServerConnection::TransportIntentKind::kConnectionClose) {
                    closeConnection(connection,
                        intent.connectionErrorCode.value_or(kProtocolFailureCode));
                } else {
                    auto* transport = wire_.transport();
                    if (transport == nullptr) {
                        closeConnection(connection, kProtocolFailureCode);
                    } else {
                        const auto reset = transport->resetStream(connection.transportId,
                            intent.token.id.streamId,
                            static_cast<std::uint64_t>(intent.streamResetErrorCode));
                        if (reset != Http3QuicServerTransport::Error::kNone &&
                            reset != Http3QuicServerTransport::Error::kNoStream) {
                            closeConnection(connection, kProtocolFailureCode);
                        }
                    }
                }
                const auto acknowledged = channel.acknowledgeIntentAfterHandoff(
                    connection.identity, intent.token);
                if (acknowledged == Http3ServerConnectionChannel::Status::kFull) {
                    connection.pendingIntentAck = intent.token;
                    break;
                }
                if (acknowledged != Http3ServerConnectionChannel::Status::kPublished) {
                    closeConnection(connection, kProtocolFailureCode);
                    break;
                }
            }
        }

        if (connection.grantReceived && !connection.bindPublished &&
            connection.revokeRequested) {
            progress = retireUnbound(worker, i) || progress;
        }

        Http3ServerConnectionChannel::RevokeAck revoke;
        const auto revokeStatus = channel.receiveRevokeAck(revoke);
        if (revokeStatus == Http3ServerConnectionChannel::Status::kReceived) {
            if (!connection.revokeRequested || connection.bindPublished ||
                revoke.identity != connection.identity) {
                std::terminate();
            }
            connection.revokeAcknowledged = true;
            progress = true;
        } else if (revokeStatus != Http3ServerConnectionChannel::Status::kEmpty &&
                   revokeStatus != Http3ServerConnectionChannel::Status::kWrongState) {
            std::terminate();
        }

        if (connection.bindPublished && connection.admissionSealedPublished &&
            !connection.drainCompleteReceived && !connection.transportRetiredPublished) {
            Http3ServerConnectionChannel::DrainComplete complete;
            const auto status = channel.receiveDrainComplete(complete);
            if (status == Http3ServerConnectionChannel::Status::kReceived) {
                if (complete.identity != connection.identity) {
                    std::terminate();
                }
                connection.drainCompleteReceived = true;
                progress = true;
            } else if (status != Http3ServerConnectionChannel::Status::kEmpty) {
                std::terminate();
            }
        }

        if (connection.transportRetiredPublished && !connection.workerFinalized) {
            Http3ServerConnectionChannel::WorkerFinalized finalized;
            const auto status = channel.receiveWorkerFinalized(finalized);
            if (status == Http3ServerConnectionChannel::Status::kReceived) {
                if (finalized.identity != connection.identity) {
                    std::terminate();
                }
                connection.workerFinalized = true;
                progress = true;
            } else if (status != Http3ServerConnectionChannel::Status::kEmpty) {
                std::terminate();
            }
        }

        // Physical transport retirement also retires every server-network-side
        // publication source. Close that gate before waiting for WorkerFinalized:
        // the worker is intentionally forbidden to publish its final record
        // until both publication gates are closed.
        if (connection.transportRetiredPublished &&
            !connection.networkPublicationsClosed) {
            const auto closed = channel.closeNetworkPublications(connection.identity);
            if (closed == Http3ServerConnectionChannel::Status::kPublished) {
                connection.networkPublicationsClosed = true;
                progress = true;
            } else if (closed != Http3ServerConnectionChannel::Status::kWrongState) {
                std::terminate();
            }
        }

        if (connection.workerFinalized) {
            if (!connection.workerFinalizedAcknowledged) {
                if (channel.acknowledgeWorkerFinalized(connection.identity) !=
                    Http3ServerConnectionChannel::Status::kPublished) {
                    std::terminate();
                }
                connection.workerFinalizedAcknowledged = true;
                progress = true;
            }
            if (connection.networkPublicationsClosed &&
                !connection.networkFinalizedPublished) {
                const auto finalized = channel.publishNetworkFinalized(connection.identity);
                if (finalized == Http3ServerConnectionChannel::Status::kPublished) {
                    connection.networkFinalizedPublished = true;
                    progress = true;
                } else if (finalized != Http3ServerConnectionChannel::Status::kWrongState) {
                    std::terminate();
                }
            }
        } else if (connection.revokeAcknowledged && !connection.accepted &&
                   !connection.networkPublicationsClosed) {
            const auto closed = channel.closeNetworkPublications(connection.identity);
            if (closed == Http3ServerConnectionChannel::Status::kPublished) {
                connection.networkPublicationsClosed = true;
                progress = true;
            } else if (closed != Http3ServerConnectionChannel::Status::kWrongState) {
                std::terminate();
            }
        }

        if (!stopping_ && channel.readyToRearm()) {
            if (channel.rearm() != Http3ServerConnectionChannel::Status::kPublished) {
                std::terminate();
            }
            connection = Connection(memory_.resource());
            notifyWorker(worker);
            progress = true;
        }
    }
    return progress;
}

bool Http3NetworkRuntime::admit(WorkerLink& worker, std::size_t index) noexcept {
    auto& connection = worker.connections[index];
    auto* transport = wire_.transport();
    if (transport == nullptr || !connection.grantReceived || connection.bindPublished ||
        connection.revokeRequested || stopping_) {
        return false;
    }

    bool progress = false;
    try {
        if (!connection.accepted) {
            const auto accepted = transport->acceptConnections(1);
            if (accepted.size == 0) {
                return false;
            }
            if (accepted.size != 1) {
                std::terminate();
            }
            connection.transportId = accepted.ids[0];
            connection.accepted = true;
            progress = true;
            try {
                // Reuse network records after a terminal event. The QUIC
                // wrapper cap bounds simultaneously live peer streams; a
                // separate lifetime cap bounds accepted stream identifiers.
                connection.streams.reserve(Http3QuicServerTransport::kMaxStreamsPerConnection);
            } catch (...) {
                progress = requestRevoke(worker, index) || progress;
                return retireUnbound(worker, index) || progress;
            }
        }

        const auto info = transport->connectionInfo(connection.transportId);
        if (!info) {
            // Handshake expiry physically retires the transport record inside
            // the QUIC owner. The worker reservation still needs a revoke ACK.
            connection.accepted = false;
            return requestRevoke(worker, index) || progress;
        }
        if (info->terminated ||
            (info->handshakeComplete &&
                (!info->h3Negotiated || info->remoteAddress.empty() || info->remotePort == 0))) {
            progress = requestRevoke(worker, index) || progress;
            return retireUnbound(worker, index) || progress;
        }
        if (!info->handshakeComplete) {
            return progress;
        }

        const auto committed = worker.channels[index]->commitAccepted(connection.identity,
            {.remoteAddress = info->remoteAddress,
                .clientCertificateSubject = info->clientCertificateSubject,
                .remotePort = info->remotePort});
        if (committed != Http3ServerConnectionChannel::Status::kPublished) {
            reportFailure(std::make_exception_ptr(
                std::runtime_error("HTTP/3 accepted connection could not publish its Bind")));
            return progress;
        }
        connection.bindPublished = true;
        return true;
    } catch (...) {
        reportFailure(std::current_exception());
        return progress;
    }
}

bool Http3NetworkRuntime::retireUnbound(WorkerLink& worker, std::size_t index) noexcept {
    auto& connection = worker.connections[index];
    if (!connection.grantReceived || connection.bindPublished) {
        return false;
    }
    bool progress = false;
    if (connection.accepted) {
        auto* transport = wire_.transport();
        if (transport == nullptr) {
            return false;
        }
        const auto info = transport->connectionInfo(connection.transportId);
        if (!info) {
            connection.accepted = false;
            progress = true;
        } else if (!wire_.outboundQuiescent()) {
            return requestRevoke(worker, index) || progress;
        } else {
            const auto retired = transport->retireConnectionLocally(connection.transportId);
            if (retired != Http3QuicServerTransport::Error::kNone &&
                retired != Http3QuicServerTransport::Error::kNoConnection) {
                return false;
            }
            connection.accepted = false;
            progress = true;
        }
    }
    return requestRevoke(worker, index) || progress;
}

bool Http3NetworkRuntime::requestRevoke(WorkerLink& worker, std::size_t index) noexcept {
    auto& connection = worker.connections[index];
    if (!connection.grantReceived || connection.bindPublished ||
        connection.revokeRequested) {
        return false;
    }
    const auto status = worker.channels[index]->revokeGrant();
    if (status != Http3ServerConnectionChannel::Status::kPublished) {
        std::terminate();
    }
    connection.revokeRequested = true;
    return true;
}

bool Http3NetworkRuntime::attach(WorkerLink& worker, std::size_t index) noexcept {
    auto& channel = *worker.channels[index];
    auto& connection = worker.connections[index];
    Http3ServerConnectionChannel::AttachResult result{
        std::in_place_type<Http3ServerConnectionChannel::AttachAck>};
    const auto status = channel.receiveAttachResult(result);
    if (status == Http3ServerConnectionChannel::Status::kEmpty) {
        return false;
    }
    if (status != Http3ServerConnectionChannel::Status::kReceived) {
        std::terminate();
    }
    const auto identity = std::visit([](const auto& value) { return value.identity; }, result);
    if (identity != connection.identity || !connection.bindPublished) {
        std::terminate();
    }
    connection.attachResolved = true;
    if (!std::holds_alternative<Http3ServerConnectionChannel::AttachAck>(result)) {
        closeConnection(connection, kProtocolFailureCode);
        return true;
    }
    if (connection.closeStarted) {
        return true;
    }
    try {
        const auto prefixes = Http3LocalCriticalStreams::create();
        auto* transport = wire_.transport();
        if (!prefixes || transport == nullptr) {
            closeConnection(connection, kProtocolFailureCode);
            return true;
        }
        connection.critical = makePmrObject<Http3CriticalStreamDriver>(
            memory_.resource(), *prefixes);
        auto planner = Http3ServerRequestAdmissionPlanner::create({
            .maxRequestsPerConnection = static_cast<std::uint64_t>(
                worker.target.maxRequestsPerConnection),
        });
        if (!planner) {
            closeConnection(connection, kProtocolFailureCode);
            return true;
        }
        connection.admissionPlanner.emplace(std::move(*planner));
        connection.output = makePmrObject<Http3ServerStreamOutput>(memory_.resource(),
            *transport, connection.transportId, memory_.resource(),
            connection.identity.epoch, connection.identity.connectionGeneration,
            Http3ServerStreamOutputConfig{
                .maxTrackedStreams = worker.target.maxRequestsPerConnection,
                .maxQueuedBlocks = worker.target.mailboxCapacity,
                .maxDriveWorkItems = 16,
                .writeTimeout = worker.target.writeTimeout,
            });
        connection.attached = true;
    } catch (...) {
        closeConnection(connection, kProtocolFailureCode);
    }
    return true;
}

bool Http3NetworkRuntime::pumpInput(WorkerLink& worker, std::size_t index) noexcept {
    auto& connection = worker.connections[index];
    auto& transport = *wire_.transport();
    bool progress = false;

    const auto critical = connection.critical->drive(
        [&transport, &connection](Http3CriticalStreamDriver::Kind) {
            return transport.openLocalUnidirectionalStream(connection.transportId);
        },
        [&transport, &connection](Http3QuicServerTransport::StreamId id,
            std::span<const char> bytes) {
            return transport.writeStream(connection.transportId, id, bytes);
        });
    if (critical == Http3CriticalStreamDriver::Result::kFatal) {
        closeConnection(connection, kProtocolFailureCode);
        return true;
    }
    progress = critical == Http3CriticalStreamDriver::Result::kProgress;
    if (connection.goawayQueued && critical == Http3CriticalStreamDriver::Result::kReady) {
        connection.goawayBytesAccepted = true;
    }

    const auto accepted = transport.acceptStreams(connection.transportId);
    if (accepted.error != Http3QuicServerTransport::Error::kNone &&
        accepted.error != Http3QuicServerTransport::Error::kStreamLimitRetry) {
        closeConnection(connection,
            accepted.error == Http3QuicServerTransport::Error::kConnectionPeerStreamLimit
                ? Http3ConnectionErrorCode::kExcessiveLoad
                : kProtocolFailureCode);
        return true;
    }
    for (std::size_t i = 0; i < accepted.size; ++i) {
        const auto streamId = accepted.streams[i].id;
        if (!accepted.streams[i].readable ||
            connection.streams.size() >= connection.streams.capacity()) {
            closeConnection(connection, Http3ConnectionErrorCode::kExcessiveLoad);
            return true;
        }
        if (isHttp3RequestStreamId(streamId)) {
            if (!connection.admissionPlanner) {
                closeConnection(connection, kProtocolFailureCode);
                return true;
            }
            const auto decision = connection.admissionPlanner->admit(streamId);
            if (decision.action != Http3ServerRequestAdmissionAction::kAdmit) {
                if (!connection.goawayQueued && !announceGoaway(worker, index)) {
                    return true;
                }
                if (!rejectRequestStream(worker, index, streamId)) {
                    return true;
                }
                progress = true;
                continue;
            }
            ++connection.admittedRequestCount;
        } else if (isHttp3ClientUnidirectionalStreamId(streamId)) {
            if (connection.peerUnidirectionalStreamCount >=
                kHttp3PeerUnidirectionalStreamAllowance) {
                closeConnection(connection, Http3ConnectionErrorCode::kExcessiveLoad);
                return true;
            }
            ++connection.peerUnidirectionalStreamCount;
        } else {
            closeConnection(connection, Http3ConnectionErrorCode::kStreamCreationError);
            return true;
        }

        connection.streams.emplace_back(memory_.resource());
        auto& stream = connection.streams.back();
        stream.id = streamId;
        stream.accepted = true;
        stream.requestStream = isHttp3RequestStreamId(streamId);
        stream.lastInputActivity = std::chrono::steady_clock::now();
        if (stream.requestStream) {
            // Duplicate only the frame-boundary state needed to observe when
            // request-header timeout should transition to body timeout.
            stream.frameTracker = makePmrObject<Http3StreamFrames>(memory_.resource(),
                Http3StreamKind::kRequest, memory_.resource());
            if (connection.admittedRequestCount == worker.target.maxRequestsPerConnection) {
                if (!announceGoaway(worker, index) || !sealAdmission(worker, index)) {
                    return true;
                }
            }
        }
        progress = true;
    }

    for (auto& stream : connection.streams) {
        if (stream.inputTerminal) {
            continue;
        }
        if (stream.pendingControl) {
            const auto sent = worker.requestMailbox.trySendControl(*stream.pendingControl);
            if (sent == Http3StreamMailbox::ControlResult::kFull) {
                continue;
            }
            if (sent == Http3StreamMailbox::ControlResult::kStopped) {
                closeConnection(connection, kServerShutdownCode);
                return true;
            }
            if (sent == Http3StreamMailbox::ControlResult::kSentNotifyPeer) {
                notifyWorker(worker);
            }
            stream.pendingControl.reset();
            if (!stream.requestStream) {
                auto* streamTransport = wire_.transport();
                const auto closed = streamTransport == nullptr
                                        ? Http3QuicServerTransport::Error::kNoConnection
                                        : streamTransport->closeStream(connection.transportId, stream.id);
                if (closed != Http3QuicServerTransport::Error::kNone &&
                    closed != Http3QuicServerTransport::Error::kNoStream) {
                    closeConnection(connection, kProtocolFailureCode);
                    return true;
                }
            }
            stream.inputTerminal = true;
            progress = true;
            continue;
        }

        if (stream.requestStream) {
            const auto timeout = stream.receivePhase == Stream::ReceivePhase::kHeaders
                                     ? worker.target.requestHeaderTimeout
                                     : worker.target.requestBodyTimeout;
            if (phaseTimeoutExpired(timeout, stream.lastInputActivity,
                    std::chrono::steady_clock::now())) {
                const auto termination = transport.terminateBidirectionalStream(
                    connection.transportId, stream.id,
                    static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestCancelled));
                if ((termination.send != Http3QuicServerTransport::Error::kNone &&
                        termination.send != Http3QuicServerTransport::Error::kNoStream) ||
                    (termination.close != Http3QuicServerTransport::Error::kNone &&
                        termination.close != Http3QuicServerTransport::Error::kNoStream)) {
                    closeConnection(connection, kProtocolFailureCode);
                    return true;
                }
                stream.frameTracker.reset();
                stream.pendingControl = Http3StreamControl{
                    .kind = Http3StreamControl::Kind::kStreamReset,
                    .id = {connection.identity.epoch,
                        connection.identity.connectionGeneration, stream.id},
                    .value = stream.receivedBytes,
                    .streamResetErrorCode = Http3ConnectionErrorCode::kRequestCancelled,
                };
                progress = true;
                continue;
            }
        }

        Http3StreamMailbox::DataReservation reservation;
        const Http3StreamMessageId messageId{connection.identity.epoch,
            connection.identity.connectionGeneration, stream.id};
        const auto reserved = worker.requestMailbox.reserveData(messageId, reservation);
        if (reserved == Http3StreamMailbox::ReservationResult::kFull ||
            reserved == Http3StreamMailbox::ReservationResult::kNoBlock) {
            break;
        }
        if (reserved != Http3StreamMailbox::ReservationResult::kReserved) {
            closeConnection(connection, kServerShutdownCode);
            return true;
        }
        auto writableBytes = reservation.writableBytes();
        auto writable = std::span<char>(reinterpret_cast<char*>(writableBytes.data()),
            writableBytes.size());
        const auto read = transport.readStream(connection.transportId, stream.id, writable);
        switch (read.status) {
            case Http3QuicServerTransport::StreamRead::Status::kData: {
                if (read.size == 0 || read.size > writable.size() ||
                    read.size > std::numeric_limits<std::uint64_t>::max() -
                                    stream.receivedBytes) {
                    reservation.abort();
                    closeConnection(connection, kProtocolFailureCode);
                    return true;
                }
                if (stream.frameTracker) {
                    // This duplicate framer only observes the first request HEADERS
                    // boundary to select header/body timeout phase. The worker's
                    // protocol owner is authoritative for framing and errors.
                    try {
                        const auto frameStatus = stream.frameTracker->feed(
                            std::span<const char>(writable.data(), read.size), false,
                            +[](void* context, Http3StreamFrameEvent event) {
                                auto& tracked = *static_cast<Stream*>(context);
                                if (event.kind == Http3StreamFrameEventKind::kHeaders &&
                                    !event.trailers && event.endFrame) {
                                    tracked.receivePhase = Stream::ReceivePhase::kBody;
                                }
                            },
                            &stream);
                        if (frameStatus != Http3StreamFrameStatus::kNeedMoreData ||
                            stream.receivePhase == Stream::ReceivePhase::kBody) {
                            stream.frameTracker.reset();
                        }
                    } catch (...) {
                        // Losing timeout-phase observation must not duplicate
                        // protocol validation or prevent forwarding bytes to the worker.
                        stream.frameTracker.reset();
                    }
                }
                const auto committed = reservation.commit(read.size);
                if (committed == Http3StreamMailbox::CommitResult::kSentNotifyPeer) {
                    notifyWorker(worker);
                } else if (committed != Http3StreamMailbox::CommitResult::kSent) {
                    closeConnection(connection, kProtocolFailureCode);
                    return true;
                }
                stream.receivedBytes += read.size;
                stream.lastInputActivity = std::chrono::steady_clock::now();
                progress = true;
                break;
            }
            case Http3QuicServerTransport::StreamRead::Status::kWouldBlock:
                reservation.abort();
                break;
            case Http3QuicServerTransport::StreamRead::Status::kFin:
                reservation.abort();
                if (stream.frameTracker) {
                    // FIN observation is only needed to retire this duplicate
                    // timeout-phase parser; the worker processes the real FIN.
                    try {
                        static_cast<void>(stream.frameTracker->feed({}, true, +[](void* context, Http3StreamFrameEvent event) {
                                auto& tracked = *static_cast<Stream*>(context);
                                if (event.kind == Http3StreamFrameEventKind::kHeaders &&
                                    !event.trailers && event.endFrame) {
                                    tracked.receivePhase = Stream::ReceivePhase::kBody;
                                } }, &stream));
                    } catch (...) {
                        // Observation failure does not change protocol handling.
                    }
                    stream.frameTracker.reset();
                }
                stream.pendingControl = Http3StreamControl{
                    .kind = Http3StreamControl::Kind::kStreamFin,
                    .id = messageId,
                    .value = stream.receivedBytes,
                };
                progress = true;
                break;
            case Http3QuicServerTransport::StreamRead::Status::kReset:
                reservation.abort();
                stream.frameTracker.reset();
                if (stream.requestStream) {
                    const auto terminated = transport.terminateBidirectionalStream(
                        connection.transportId, stream.id,
                        static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestCancelled));
                    if (terminated.close != Http3QuicServerTransport::Error::kNone &&
                        terminated.close != Http3QuicServerTransport::Error::kNoStream) {
                        closeConnection(connection, kProtocolFailureCode);
                        return true;
                    }
                }
                stream.pendingControl = Http3StreamControl{
                    .kind = Http3StreamControl::Kind::kStreamReset,
                    .id = messageId,
                    .value = stream.receivedBytes,
                    .streamResetErrorCode = static_cast<Http3ConnectionErrorCode>(
                        read.peerResetErrorCode.value_or(static_cast<std::uint64_t>(
                            Http3ConnectionErrorCode::kRequestCancelled))),
                };
                progress = true;
                break;
            default:
                reservation.abort();
                closeConnection(connection, kProtocolFailureCode);
                return true;
        }
    }
    std::erase_if(connection.streams, [](const Stream& stream) {
        return stream.inputTerminal;
    });
    return progress;
}

bool Http3NetworkRuntime::announceGoaway(WorkerLink& worker, std::size_t index) noexcept {
    auto& connection = worker.connections[index];
    if (connection.goawayQueued) {
        return true;
    }
    if (!connection.admissionPlanner || !connection.critical) {
        closeConnection(connection, kProtocolFailureCode);
        return false;
    }
    (void)connection.admissionPlanner->announceGoaway();
    if (!connection.critical->queueGoaway(connection.admissionPlanner->goawayId())) {
        closeConnection(connection, kProtocolFailureCode);
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        drainTimeout_);
    connection.drainDeadline = timeout > std::chrono::steady_clock::time_point::max() - now
                                   ? std::chrono::steady_clock::time_point::max()
                                   : now + timeout;
    connection.goawayQueued = true;
    notifyWorker(worker);
    return true;
}

bool Http3NetworkRuntime::sealAdmission(WorkerLink& worker, std::size_t index) noexcept {
    auto& connection = worker.connections[index];
    if (connection.admissionSealedPublished) {
        return true;
    }
    if (!connection.goawayQueued || !connection.admissionPlanner ||
        connection.admittedRequestCount != worker.target.maxRequestsPerConnection) {
        closeConnection(connection, kProtocolFailureCode);
        return false;
    }
    const auto sealed = worker.channels[index]->publishAdmissionSealed(connection.identity,
        worker.target.maxRequestsPerConnection, connection.admissionPlanner->goawayId());
    if (sealed != Http3ServerConnectionChannel::Status::kPublished) {
        closeConnection(connection, kProtocolFailureCode);
        return false;
    }
    connection.admissionSealedPublished = true;
    notifyWorker(worker);
    return true;
}

bool Http3NetworkRuntime::rejectRequestStream(WorkerLink& worker, std::size_t index,
    Http3QuicServerTransport::StreamId streamId) noexcept {
    auto& connection = worker.connections[index];
    if (connection.rejectedRequestCount >= kHttp3PostGoawayRequestAllowance) {
        closeConnection(connection, Http3ConnectionErrorCode::kExcessiveLoad);
        return false;
    }
    auto* transport = wire_.transport();
    if (transport == nullptr) {
        closeConnection(connection, kProtocolFailureCode);
        return false;
    }
    const auto termination = transport->terminateBidirectionalStream(connection.transportId,
        streamId, static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestRejected));
    if ((termination.send != Http3QuicServerTransport::Error::kNone &&
            termination.send != Http3QuicServerTransport::Error::kNoStream) ||
        (termination.close != Http3QuicServerTransport::Error::kNone &&
            termination.close != Http3QuicServerTransport::Error::kNoStream)) {
        closeConnection(connection, kProtocolFailureCode);
        return false;
    }
    ++connection.rejectedRequestCount;
    return true;
}

bool Http3NetworkRuntime::pumpResponses(WorkerLink& worker) noexcept {
    bool progress = false;
    worker.responseMailboxDrained = false;
    for (;;) {
        if (worker.pendingResponseControl) {
            auto* connection = findConnection(worker, worker.pendingResponseControl->id);
            if (connection == nullptr || connection->output == nullptr) {
                worker.pendingResponseControl.reset();
                progress = true;
            } else {
                const auto result = connection->output->acceptControl(
                    *worker.pendingResponseControl);
                if (result.status == Http3ServerStreamOutput::Status::kBackpressured) {
                    break;
                }
                if (result.status != Http3ServerStreamOutput::Status::kAccepted &&
                    result.status != Http3ServerStreamOutput::Status::kFinDeferred &&
                    result.status != Http3ServerStreamOutput::Status::kFinished &&
                    result.status != Http3ServerStreamOutput::Status::kDuplicateFin &&
                    result.status != Http3ServerStreamOutput::Status::kClosedStream) {
                    closeConnection(*connection, kProtocolFailureCode);
                }
                worker.pendingResponseControl.reset();
                progress = true;
            }
        }
        if (worker.pendingResponse) {
            auto* connection = findConnection(worker, worker.pendingResponse->id());
            if (connection == nullptr || connection->output == nullptr) {
                worker.pendingResponse->release();
                worker.pendingResponse.reset();
                progress = true;
            } else {
                const auto result = connection->output->acceptData(*worker.pendingResponse);
                if (result.status == Http3ServerStreamOutput::Status::kBackpressured) {
                    break;
                }
                if (result.status != Http3ServerStreamOutput::Status::kAccepted &&
                    result.status != Http3ServerStreamOutput::Status::kClosedStream) {
                    worker.pendingResponse->release();
                    closeConnection(*connection, kProtocolFailureCode);
                }
                worker.pendingResponse.reset();
                progress = true;
            }
        }
        if (!worker.pendingResponseControl) {
            Http3StreamControl control;
            if (worker.target.server->responseMailbox().tryReceiveControl(control)) {
                worker.pendingResponseControl = control;
                progress = true;
                continue;
            }
        }
        if (!worker.pendingResponse) {
            Http3StreamMailbox::BorrowedBlock block;
            if (worker.target.server->responseMailbox().tryReceive(block)) {
                worker.pendingResponse.emplace(std::move(block));
                progress = true;
                continue;
            }
        }
        if (!worker.target.server->responseMailbox().finishDrain()) {
            worker.responseMailboxDrained = true;
            break;
        }
    }
    return progress;
}

bool Http3NetworkRuntime::pumpOutput(WorkerLink& worker, std::size_t index) noexcept {
    auto& connection = worker.connections[index];
    if (connection.output == nullptr) {
        return false;
    }
    connection.output->notifyTransportActivity();
    const auto result = connection.output->drive();
    if (result.status == Http3ServerStreamOutput::Status::kTransportError ||
        result.status == Http3ServerStreamOutput::Status::kFinalSizeError ||
        result.status == Http3ServerStreamOutput::Status::kCapacityExhausted ||
        result.status == Http3ServerStreamOutput::Status::kUnsafeToRelease) {
        closeConnection(connection, kProtocolFailureCode);
    }
    return result.madeProgress || result.needsReschedule;
}

bool Http3NetworkRuntime::retire(WorkerLink& worker, std::size_t index) noexcept {
    auto& connection = worker.connections[index];
    if (!connection.accepted || !connection.bindPublished || !connection.attachResolved ||
        connection.transportRetiredPublished) {
        return false;
    }

    if (connection.goawayQueued && !connection.closeStarted &&
        !connection.gracefulCloseStarted) {
        const auto now = std::chrono::steady_clock::now();
        if (connection.drainDeadline && now >= *connection.drainDeadline) {
            // The deadline also bounds an unsealed admission window: without
            // all N eligible low-ID requests, graceful retirement is forbidden.
            closeConnection(connection, kServerShutdownCode);
        } else if (connection.admissionSealedPublished && connection.goawayBytesAccepted &&
                   connection.drainCompleteReceived && worker.responseMailboxDrained &&
                   !worker.pendingResponse && !worker.pendingResponseControl &&
                   connection.output != nullptr && connection.output->liveStreamCount() == 0) {
            connection.gracefulCloseStarted = true;
        }
    }
    if (connection.gracefulCloseStarted && connection.drainDeadline &&
        std::chrono::steady_clock::now() >= *connection.drainDeadline) {
        connection.gracefulCloseAbandoned = true;
    }
    if (!connection.closeStarted && !connection.gracefulCloseStarted) {
        return false;
    }

    auto* transport = wire_.transport();
    if (transport == nullptr) {
        return false;
    }
    bool forceLocalRetirement = connection.gracefulCloseStarted &&
                                connection.gracefulCloseAbandoned;
    Http3QuicServerTransport::ConnectionCloseResult close{};
    if (connection.gracefulCloseStarted && !forceLocalRetirement) {
        close = transport->requestGracefulConnectionClose(connection.transportId);
        if (close.status == Http3QuicServerTransport::ConnectionCloseStatus::kFailure) {
            connection.gracefulCloseAbandoned = true;
            forceLocalRetirement = true;
        } else if (close.status == Http3QuicServerTransport::ConnectionCloseStatus::kPending) {
            return false;
        } else if (close.status == Http3QuicServerTransport::ConnectionCloseStatus::kConflict) {
            std::terminate();
        } else if (close.status == Http3QuicServerTransport::ConnectionCloseStatus::kNoConnection) {
            forceLocalRetirement = true;
        }
    } else if (connection.closeStarted && !forceLocalRetirement) {
        close = transport->requestConnectionClose(connection.transportId,
            connection.closeErrorCode.value_or(kServerShutdownCode));
        if (close.status == Http3QuicServerTransport::ConnectionCloseStatus::kConflict) {
            // closeConnection() latches the rapid-path reason before the first
            // transport call; a different retry would violate owner state.
            std::terminate();
        }
        if (close.status == Http3QuicServerTransport::ConnectionCloseStatus::kPending) {
            return false;
        }
    }

    if (!wire_.outboundQuiescent()) {
        return false;
    }
    if (connection.output != nullptr) {
        const auto stopped = connection.output->stop();
        if (stopped.status == Http3ServerStreamOutput::Status::kUnsafeToRelease) {
            return false;
        }
        connection.output.reset();
    }
    connection.critical.reset();
    const auto retired = transport->retireConnectionLocally(connection.transportId);
    if (retired != Http3QuicServerTransport::Error::kNone &&
        retired != Http3QuicServerTransport::Error::kNoConnection) {
        return false;
    }
    if (worker.channels[index]->publishTransportRetired(connection.identity) !=
        Http3ServerConnectionChannel::Status::kPublished) {
        std::terminate();
    }
    connection.transportRetiredPublished = true;
    notifyWorker(worker);
    return true;
}

Http3NetworkRuntime::Connection* Http3NetworkRuntime::findConnection(
    WorkerLink& worker, Http3StreamMessageId id) noexcept {
    const auto found = std::ranges::find_if(worker.connections, [&id](const Connection& value) {
        return value.attached && value.identity.epoch == id.epoch &&
               value.identity.connectionGeneration == id.connectionGeneration;
    });
    return found == worker.connections.end() ? nullptr : &*found;
}

void Http3NetworkRuntime::closeConnection(Connection& connection,
    Http3ConnectionErrorCode reason) noexcept {
    if (!connection.accepted || connection.closeStarted) {
        return;
    }
    if (connection.gracefulCloseStarted) {
        // Once SSL_shutdown_ex has entered graceful mode its flags cannot be
        // changed. The owner instead waits for all borrowed output to quiesce
        // and performs local retirement.
        connection.gracefulCloseAbandoned = true;
        return;
    }
    connection.closeStarted = true;
    connection.closeErrorCode = reason;
    if (auto* transport = wire_.transport(); transport != nullptr) {
        (void)transport->requestConnectionClose(connection.transportId, reason);
    }
}

void Http3NetworkRuntime::notifyWorker(WorkerLink& worker) noexcept {
    (void)worker.target.server->notification().notify();
}

}  // namespace ruvia::detail
