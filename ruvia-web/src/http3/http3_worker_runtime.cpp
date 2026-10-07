#include "ruvia/web/detail/http3/http3_worker_runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/http/Http3PeerStreams.h"
#include "ruvia/http/Http3StreamFrames.h"
#include "ruvia/web/detail/http3/Http3QuicSocketAddress.h"
#include "ruvia/web/detail/http3/Http3WorkerServer.h"
#include "ruvia/web/detail/server/HttpServerListener.h"
#include "ruvia/web/detail/server/HttpServerOptionsValidation.h"

namespace ruvia::detail {
namespace {

constexpr auto kServerShutdownCode = Http3ConnectionErrorCode::kNoError;
constexpr auto kProtocolFailureCode = Http3ConnectionErrorCode::kInternalError;
constexpr auto kMonitorInterval = std::chrono::milliseconds(10);
constexpr std::size_t kInputStreamPumpBudget = 64;
constexpr std::size_t kResponseMailboxPumpBudget = 64;

bool phaseTimeoutExpired(std::optional<std::chrono::milliseconds> timeout,
    std::chrono::steady_clock::time_point lastActivity,
    std::chrono::steady_clock::time_point now) noexcept {
    return timeout && now >= lastActivity && now - lastActivity >= *timeout;
}

std::size_t connection_capacity(const http3_worker_runtime::worker_target& worker) {
    if (worker.server == nullptr || worker.max_connections == 0 ||
        worker.mailbox_capacity == 0 || worker.max_requests_per_connection == 0) {
        throw std::invalid_argument("invalid HTTP/3 worker target");
    }
    return worker.max_connections;
}

ruvia::quic_server_config transport_config(
    const http3_worker_runtime::worker_target& worker, ruvia::quic_cid_partition partition) {
    ruvia::quic_server_config config;
    config.max_active_connections = connection_capacity(worker);
    config.max_pending_connections = config.max_active_connections;
    config.cid_partition = partition;
    config.limits.max_lifetime_peer_streams =
        http3TransportLifetimeStreamCapacity(worker.max_requests_per_connection);
    config.local_transport_parameters.idle_timeout_ms =
        worker.idle_timeout ? static_cast<std::uint64_t>(worker.idle_timeout->count()) : 0;
    config.local_transport_parameters.max_datagram_frame_size = 65536;
    return config;
}

}  // namespace

bool queue_http3_initial_offer(
    std::pmr::vector<ruvia::quic_initial_offer>& pending_offers,
    const ruvia::quic_initial_offer& offer) noexcept {
    if (std::ranges::any_of(pending_offers, [&offer](const ruvia::quic_initial_offer& pending) {
            return pending.offer_id == offer.offer_id;
        }) ||
        pending_offers.size() == pending_offers.capacity()) {
        return false;
    }
    pending_offers.push_back(offer);
    return true;
}

Http3DatagramEndpoint::pump_result send_http3_version_negotiation(
    ruvia::quic_server& server, ruvia::quic_version_negotiation_plan& plan,
    Http3DatagramEndpoint& endpoint, std::span<std::byte> packet_buffer) {
    if (endpoint.send_in_flight()) {
        return Http3DatagramEndpoint::pump_result::idle;
    }
    packet_buffer = endpoint.packet_buffer(packet_buffer);
    if (packet_buffer.empty()) {
        return Http3DatagramEndpoint::pump_result::idle;
    }
    const auto packet = server.write_version_negotiation(plan, packet_buffer);
    if (packet.status == ruvia::quic_operation_status::would_block) {
        return Http3DatagramEndpoint::pump_result::idle;
    }
    if (packet.status != ruvia::quic_operation_status::accepted || packet.size == 0 ||
        packet.size > packet_buffer.size()) {
        throw std::runtime_error("QUIC server failed to serialize Version Negotiation");
    }
    const auto source = to_udp_endpoint(from_quic_address(packet.local));
    const auto peer = to_udp_endpoint(from_quic_address(packet.peer));
    if (!source || !peer) {
        throw std::runtime_error("QUIC Version Negotiation returned invalid UDP addresses");
    }
    const auto sent = endpoint.send_datagram(
        std::span<const std::byte>(packet_buffer).first(packet.size), *source, *peer);
    if (sent == Http3DatagramEndpoint::pump_result::error) {
        throw std::system_error(endpoint.error(), "send HTTP/3 Version Negotiation packet");
    }
    return sent;
}

http3_worker_runtime::worker_link::worker_link(std::pmr::memory_resource* resource,
    http3_worker_runtime& network, worker_target configured)
    : target(configured),
      requestMailbox(configured.mailbox_capacity, configured.mailbox_capacity,
          configured.mailbox_capacity, resource,
          Http3StreamMailboxCapacityNotifier{&network, &http3_worker_runtime::networkWake}),
      channels(resource),
      channelViews(resource),
      connections(resource) {
    channels.reserve(target.max_connections);
    channelViews.reserve(target.max_connections);
    connections.reserve(target.max_connections);
    const Http3ServerConnectionChannel::Notification networkNotification{
        &network, &http3_worker_runtime::networkWake};
    const Http3ServerConnectionChannel::Notification workerNotification{
        target.server, &http3_worker_runtime::workerWake};
    for (std::size_t i = 0; i < target.max_connections; ++i) {
        channels.push_back(makePmrObject<Http3ServerConnectionChannel>(
            resource, networkNotification, workerNotification,
            network.localSettings_.h3Datagram ? resource : nullptr));
        channelViews.push_back(channels.back().get());
        connections.emplace_back(resource);
    }
}

http3_worker_runtime::http3_worker_runtime(ruvia::WorkerRuntimeContext& runtime,
    asio::ip::udp::endpoint local, const HttpServerListenerDefinition::Tls& tls_config,
    const Http3ListenConfig& http3_config, worker_target worker,
    http3_datagram_channel& datagrams, ruvia::quic_cid_partition partition,
    failure_notification failure)
    : ioContext_(runtime.ioContext()),
      ownerThread_(std::this_thread::get_id()),
      memory_(),
      bindAddress_(local.address()),
      tls_(tls_config, memory_.resource()),
      wire_(ioContext_, datagrams, local, tls_, transport_config(worker, partition),
          memory_.resource(), {this, [](void* context, http3_quic_server_transport& transport, Http3DatagramEndpoint& endpoint) noexcept {
                                   auto& self = *static_cast<http3_worker_runtime*>(context);
                                   const bool progress = self.protocolPump(&transport, &endpoint);
                                   if (self.failure_) {
                                       return Http3QuicWireOwner::ProtocolPumpResult::kFatal;
                                   }
                                   return progress
                                              ? Http3QuicWireOwner::ProtocolPumpResult::kProgress
                                              : Http3QuicWireOwner::ProtocolPumpResult::kIdle;
                               }}),
      worker_(nullptr, PmrObjectDeleter<worker_link>{memory_.resource()}),
      datagrams_(&datagrams),
      pendingOffers_(memory_.resource()),
      monitorTimer_(ioContext_),
      failureNotification_(failure),
      drainTimeout_(http3_config.drainTimeout),
      handshakeTimeout_(http3_config.handshakeTimeout),
      localSettings_{.qpackMaxTableCapacity = http3_config.qpack.maxTableCapacity, .qpackBlockedStreams = http3_config.qpack.maxBlockedStreams, .enableConnectProtocol = true, .h3Datagram = true} {
    try {
        if (drainTimeout_ <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("HTTP/3 drain timeout must be greater than zero");
        }
        pendingOffers_.reserve(connection_capacity(worker));
        worker_ = makePmrObject<worker_link>(memory_.resource(), memory_.resource(), *this, worker);
        wire_.prepare();
        wire_.deferTransportRetirement();
        // The caller owns constructor rollback until the final nonthrowing step.
        datagrams.worker_start();
    } catch (...) {
        // Detach before the caller publishes its single cold rollback ACK.
        wire_.requestStop();
        wire_.pollStop();
        if (!wire_.stopStatus().complete()) {
            std::terminate();
        }
        datagrams_ = nullptr;
        throw;
    }
}

http3_worker_runtime::~http3_worker_runtime() {
    requireOwnerThread();
    if (running_ || monitorScheduled_ || !protocol_drained() || datagrams_ != nullptr) {
        std::terminate();
    }
}

void http3_worker_runtime::stage() {
    requireOwnerThread();
    if (staged_) {
        throw std::logic_error("HTTP/3 worker link already staged");
    }
    auto& worker = *worker_;
    if (!worker.target.server->stageInstall({
            .requestMailbox = &worker.requestMailbox,
            .channels = worker.channelViews,
            .networkWake = {this, &networkWake},
        })) {
        throw std::runtime_error("failed to stage HTTP/3 worker link");
    }
    staged_ = true;
}

void http3_worker_runtime::start() {
    requireOwnerThread();
    if (!staged_ || running_ || stopping_) {
        throw std::logic_error("HTTP/3 worker runtime cannot start in this state");
    }
    running_ = true;
    wire_.start();
    wire_.requestDrive();
    scheduleMonitor();
}

void http3_worker_runtime::wake() noexcept {
    requireOwnerThread();
    if (datagrams_ != nullptr) {
        datagrams_->wake_worker();
    }
}

void http3_worker_runtime::stop() noexcept {
    requireOwnerThread();
    const bool alreadyStopping = stopping_;
    requestStopOnOwner();
    if (!alreadyStopping) {
        (void)protocolPump();
    }
    wire_.pollStop();
    if (!protocol_drained()) {
        scheduleMonitor();
    }
    wake();
}

bool http3_worker_runtime::protocol_drained() const noexcept {
    requireOwnerThread();
    if (!stopping_) {
        return false;
    }
    const bool channels_done = std::ranges::all_of(worker_->channels,
        [](const auto& channel) { return channel->readyToDestroy(); });
    return channels_done && worker_->target.server->drained() &&
           wire_.stopStatus().complete() && !monitorScheduled_;
}

bool http3_worker_runtime::drained() const noexcept {
    return protocol_drained() && datagrams_ == nullptr &&
           (!datagram_run_started_ || datagram_run_retired_);
}

asio::ip::udp::endpoint http3_worker_runtime::local_endpoint() const {
    requireOwnerThread();
    return {bindAddress_, wire_.boundPort()};
}

Task<void> http3_worker_runtime::run_datagrams() {
    requireOwnerThread();
    if (datagram_run_started_ || datagrams_ == nullptr || abandon_requested_) {
        std::terminate();
    }
    datagram_run_started_ = true;
    try {
        while (!protocol_drained()) {
            if (datagrams_->acceptor_closed()) {
                requestStopOnOwner();
            }
            wire_.poll_datagrams();
            if (const auto failure = wire_.failure()) {
                reportFailure(failure);
            }
            if (stopping_) {
                (void)protocolPump();
                wire_.pollStop();
                scheduleMonitor();
            } else if (running_) {
                wire_.requestDrive();
            }
            if (!protocol_drained()) {
                if (co_await datagrams_->worker_notification().wait() ==
                    WorkerNotificationWaitStatus::kClosed) {
                    abandon_requested_ = true;
                    stop();
                    break;
                }
            }
        }
    } catch (...) {
        // A failed native wait must not leave the acceptor awaiting an ACK.
        // The owner monitor continues ordered protocol retirement without it.
        abandon_requested_ = true;
        reportFailure(std::current_exception());
    }
    datagram_run_retired_ = true;
    finish_datagrams();
}

void http3_worker_runtime::abandon_before_launch() noexcept {
    requireOwnerThread();
    if (datagram_run_started_) {
        std::terminate();
    }
    abandon_requested_ = true;
    stop();
    finish_datagrams();
}

void http3_worker_runtime::finish_datagrams() noexcept {
    if (datagrams_ == nullptr || !protocol_drained() ||
        (datagram_run_started_ && !datagram_run_retired_)) {
        return;
    }
    // wire stop has detached its endpoint; no borrowed channel survives ACK.
    auto* channel = std::exchange(datagrams_, nullptr);
    channel->worker_close();
}

void http3_worker_runtime::networkWake(void* context) noexcept {
    static_cast<http3_worker_runtime*>(context)->wake();
}

void http3_worker_runtime::workerWake(void* context) noexcept {
    auto& worker = *static_cast<Http3WorkerServer*>(context);
    (void)worker.notification().notify();
}

void http3_worker_runtime::requireOwnerThread() const noexcept {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
}

void http3_worker_runtime::requestStopOnOwner() noexcept {
    if (stopping_) {
        return;
    }
    stopping_ = true;
    worker_->target.server->requestStop();
    auto& worker = *worker_;
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

void http3_worker_runtime::reportFailure(std::exception_ptr failure) noexcept {
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

void http3_worker_runtime::scheduleMonitor() noexcept {
    requireOwnerThread();
    if (monitorScheduled_ || (!running_ && !stopping_) || protocol_drained()) {
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
        if (!protocol_drained()) {
            // A failed monitor initiation must not leave joined owners waiting
            // on a wire callback that can no longer make ordered progress.
            std::terminate();
        }
    }
}

void http3_worker_runtime::monitor(const asio::error_code& error) noexcept {
    requireOwnerThread();
    monitorScheduled_ = false;
    if (error) {
        if (error != asio::error::operation_aborted) {
            try {
                throw std::system_error(error, "HTTP/3 worker monitor timer");
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
    if (!protocol_drained()) {
        scheduleMonitor();
    }
    if (abandon_requested_) {
        finish_datagrams();
    } else {
        wake();
    }
}

bool http3_worker_runtime::protocolPump(http3_quic_server_transport* transport,
    Http3DatagramEndpoint* endpoint) noexcept {
    requireOwnerThread();
    if (!running_ && !stopping_) {
        return false;
    }

    try {
        auto& worker = worker_;
        bool anyProgress = false;
        bool exhaustedBudget = true;
        transportActivityForPump_ = wire_.consumeTransportActivity();
        if (transport == nullptr) {
            transport = wire_.transport();
        }
        if (!stopping_ && transport != nullptr && endpoint != nullptr) {
            const auto now = std::chrono::steady_clock::now();
            (void)transport->server().handle_expiry(now);
            if (const auto received = endpoint->receive_slot()) {
                const auto local = to_http3_quic_datagram_address(received->local_destination);
                const auto peer = to_http3_quic_datagram_address(received->peer);
                if (local && peer) {
                    ruvia::quic_server_route route;
                    try {
                        route = transport->route_datagram(received->bytes, *local, *peer);
                    } catch (const ruvia::quic_error& error) {
                        if (error.code() != ruvia::quic_error_code::protocol_failure) {
                            throw;
                        }
                    }
                    if (route.kind == ruvia::quic_server_route_kind::initial_offer) {
                        (void)queue_http3_initial_offer(pendingOffers_, route.offer);
                    } else if (route.kind == ruvia::quic_server_route_kind::version_negotiation) {
                        if (send_http3_version_negotiation(transport->server(),
                                route.version_negotiation, *endpoint, {}) ==
                            Http3DatagramEndpoint::pump_result::pending) {
                            anyProgress = true;
                        }
                    } else if (route.kind == ruvia::quic_server_route_kind::existing_connection) {
                        const ruvia::quic_datagram_view datagram{
                            received->bytes, to_quic_address(*local), to_quic_address(*peer)};
                        try {
                            (void)transport->server().receive(route.connection, datagram, now);
                        } catch (const ruvia::quic_error&) {
                            for (std::size_t index = 0; index < worker->connections.size(); ++index) {
                                auto& connection = worker->connections[index];
                                if (connection.accepted && connection.transportId == route.connection) {
                                    if (connection.bindPublished) {
                                        closeConnection(connection, kProtocolFailureCode);
                                    } else {
                                        transport->retire(route.connection);
                                        connection.accepted = false;
                                        (void)requestRevoke(*worker, index);
                                    }
                                }
                            }
                        }
                    }
                }
                const auto consumed = endpoint->consume_receive();
                if (consumed == Http3DatagramEndpoint::pump_result::error) {
                    throw std::system_error(endpoint->error(), "consume HTTP/3 UDP receive slot");
                }
            }
            for (std::size_t index = 0; index < worker->connections.size(); ++index) {
                auto& connection = worker->connections[index];
                if (!connection.accepted) {
                    continue;
                }
                try {
                    const auto info = transport->server().connection(connection.transportId).info();
                    if (connection.handshakeDeadline && now >= *connection.handshakeDeadline &&
                        !info.quic_handshake_complete) {
                        if (connection.bindPublished) {
                            closeConnection(connection, kProtocolFailureCode);
                        } else {
                            (void)retireUnbound(*worker, index);
                        }
                        continue;
                    }
                    if (info.state == ruvia::quic_connection_state::failed ||
                        info.state == ruvia::quic_connection_state::retired) {
                        if (connection.bindPublished) {
                            closeConnection(connection, kProtocolFailureCode);
                        } else {
                            transport->retire(connection.transportId);
                            connection.accepted = false;
                            (void)requestRevoke(*worker, index);
                        }
                    }
                } catch (const ruvia::quic_error&) {
                    if (connection.bindPublished) {
                        closeConnection(connection, kProtocolFailureCode);
                    } else {
                        transport->retire(connection.transportId);
                        connection.accepted = false;
                        (void)requestRevoke(*worker, index);
                    }
                }
            }
        }
        for (std::size_t pass = 0; pass < pumpBudget_; ++pass) {
            const bool progress = pumpWorker(*worker_);
            if (!progress) {
                exhaustedBudget = false;
                break;
            }
            anyProgress = true;
        }

        if (!stopping_ && transport != nullptr && endpoint != nullptr &&
            !endpoint->send_in_flight()) {
            for (auto& connection : worker_->connections) {
                if (!connection.accepted) {
                    continue;
                }
                try {
                    auto& quic = transport->server().connection(connection.transportId);
                    const auto packet_buffer = endpoint->packet_buffer({});
                    if (packet_buffer.empty()) {
                        break;
                    }
                    const auto packet = quic.write_packet(packet_buffer,
                        std::chrono::steady_clock::now());
                    if (packet.size == 0) {
                        continue;
                    }
                    const auto source = to_udp_endpoint(from_quic_address(packet.local));
                    const auto peer = to_udp_endpoint(from_quic_address(packet.peer));
                    if (!source || !peer || packet.size > packet_buffer.size()) {
                        closeConnection(connection, kProtocolFailureCode);
                        continue;
                    }
                    const auto sent = endpoint->send_datagram(
                        std::span<const std::byte>(packet_buffer).first(packet.size),
                        *source, *peer);
                    if (sent == Http3DatagramEndpoint::pump_result::error) {
                        throw std::system_error(endpoint->error(), "send HTTP/3 QUIC packet");
                    }
                    anyProgress = true;
                    break;
                } catch (const ruvia::quic_error&) {
                    closeConnection(connection, kProtocolFailureCode);
                }
            }
        }

        if (stopping_) {
            const bool channelsDone = std::ranges::all_of(worker_->channels,
                [](const auto& channel) { return channel->readyToDestroy(); });
            const bool workersDone = worker_->target.server->drained();
            if (channelsDone && workersDone) {
                wire_.releaseTransportRetirement();
            }
            if (channelsDone && !wire_.stopStatus().stopping) {
                wire_.requestStop();
                anyProgress = true;
            }
            wire_.pollStop();
            if (channelsDone && wire_.stopStatus().complete() && workersDone) {
                running_ = false;
            }
        } else if (exhaustedBudget) {
            // The QUIC wire owner bounds each turn. Queue a coalesced follow-up
            // so a long run of ready mailbox work cannot strand the connection
            // after the last UDP/timer completion.
            wake();
        }
        transportActivityForPump_ = false;
        return anyProgress;
    } catch (...) {
        transportActivityForPump_ = false;
        reportFailure(std::current_exception());
        return false;
    }
}

bool http3_worker_runtime::pumpWorker(worker_link& worker) noexcept {
    bool progress = false;
    try {
        progress = pumpChannels(worker);
    } catch (...) {
        reportFailure(std::current_exception());
        return false;
    }
    for (std::size_t i = 0; i < worker.connections.size(); ++i) {
        auto& connection = worker.connections[i];
        if (connection.attached && !connection.closeStarted &&
            !connection.gracefulCloseStarted) {
            auto* transport = wire_.transport();
            if (transport == nullptr) {
                closeConnection(connection, kServerShutdownCode);
                progress = true;
            } else {
                try {
                    const auto state = transport->server().connection(connection.transportId).info().state;
                    if (state == ruvia::quic_connection_state::failed ||
                        state == ruvia::quic_connection_state::retired ||
                        state == ruvia::quic_connection_state::closing ||
                        state == ruvia::quic_connection_state::draining) {
                        closeConnection(connection, kServerShutdownCode);
                        progress = true;
                    } else {
                        progress = pumpInput(worker, i) || progress;
                        progress = pump_datagrams(worker, i) || progress;
                        progress = pumpOutput(worker, i) || progress;
                    }
                } catch (const ruvia::quic_error&) {
                    closeConnection(connection, kProtocolFailureCode);
                    progress = true;
                } catch (...) {
                    reportFailure(std::current_exception());
                    return progress;
                }
            }
        }
    }
    try {
        progress = pumpResponses(worker) || progress;
    } catch (...) {
        reportFailure(std::current_exception());
        return progress;
    }
    for (std::size_t i = 0; i < worker.connections.size(); ++i) {
        try {
            progress = retire(worker, i) || progress;
        } catch (const ruvia::quic_error&) {
            closeConnection(worker.connections[i], kProtocolFailureCode);
            progress = true;
        } catch (...) {
            reportFailure(std::current_exception());
            return progress;
        }
    }
    return progress;
}

bool http3_worker_runtime::pumpChannels(worker_link& worker) noexcept {
    bool progress = false;
    for (std::size_t i = 0; i < worker.channels.size(); ++i) {
        auto& channel = *worker.channels[i];
        auto& connection = worker.connections[i];
        if (!connection.grantReceived && !connection.networkPublicationsClosed) {
            Http3ServerConnectionChannel::Identity identity;
            const auto status = channel.peekGrant(identity);
            if (status == Http3ServerConnectionChannel::Status::kReceived) {
                connection.identity = identity;
                connection.hasLastIdentity = true;
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
        // The channel accepts worker intents during the AttachAck publication
        // window, but the same-worker wire driver consumes that ACK first.

        if (connection.pendingIntentAck) {
            const auto acknowledged = channel.acknowledgeIntentAfterHandoff(
                connection.identity, connection.pendingIntentAck->token,
                connection.pendingIntentAck->settlement,
                connection.pendingIntentAck->pushStream);
            if (acknowledged == Http3ServerConnectionChannel::Status::kPublished) {
                connection.pendingIntentAck.reset();
                progress = true;
            } else if (acknowledged != Http3ServerConnectionChannel::Status::kFull) {
                closeConnection(connection, kProtocolFailureCode);
            }
        }
        // Keep draining this generation's intent ledger after physical
        // retirement. Those intents are superseded by the retired transport,
        // but still require exact-token ACKs so the worker can retire without
        // losing scheduler/connection bookkeeping.
        if (connection.bindPublished && connection.attachResolved &&
            !connection.pendingIntentAck) {
            Http3ServerConnectionChannel::TransportIntent intent;
            for (;;) {
                const auto status = channel.receiveIntent(intent);
                if (status == Http3ServerConnectionChannel::Status::kEmpty ||
                    status == Http3ServerConnectionChannel::Status::kWrongState) {
                    // WorkerFinalized closes this producer gate before the wire
                    // driver consumes that record; no further intents remain.
                    break;
                }
                if (status != Http3ServerConnectionChannel::Status::kReceived ||
                    intent.token.id.epoch != connection.identity.epoch ||
                    intent.token.id.connectionGeneration !=
                        connection.identity.connectionGeneration) {
                    std::terminate();
                }
                progress = true;
                const auto settlement = connection.transportRetiredPublished
                                            ? Http3ServerConnectionChannel::IntentSettlement::
                                                  kTransportRetiredSuperseded
                                            : Http3ServerConnectionChannel::IntentSettlement::
                                                  kExecutedHandoff;
                std::optional<Http3ServerConnection::PushStreamOpenResult> pushStream;
                if (intent.token.kind == Http3ServerConnection::TransportIntentKind::kOpenPushStream) {
                    pushStream.emplace();
                    if (settlement == Http3ServerConnectionChannel::IntentSettlement::kTransportRetiredSuperseded || stopping_) {
                        pushStream->status = Http3ServerConnection::PushStreamOpenResult::Status::kStopped;
                    } else if (intent.token.id.pushId && connection.output != nullptr && connection.pushStreams.size() < connection.pushStreams.capacity()) {
                        try {
                            auto* transport = wire_.transport();
                            if (transport == nullptr) {
                                pushStream->status = Http3ServerConnection::PushStreamOpenResult::Status::kStopped;
                            } else {
                                auto& quic = transport->server().connection(connection.transportId);
                                const auto opened = quic.open_stream(true);
                                if (opened.status == ruvia::quic_operation_status::accepted) {
                                    const auto registered = connection.output->registerPushStream(
                                        opened.stream_id, *intent.token.id.pushId);
                                    if (registered.status == Http3ServerStreamOutput::Status::kAccepted) {
                                        *pushStream = {
                                            .status = Http3ServerConnection::PushStreamOpenResult::Status::kOpened,
                                            .streamId = opened.stream_id};
                                        connection.pushStreams.push_back(
                                            {opened.stream_id, *intent.token.id.pushId});
                                    } else {
                                        (void)quic.close_stream(opened.stream_id);
                                    }
                                } else if (opened.status != ruvia::quic_operation_status::would_block &&
                                           opened.status != ruvia::quic_operation_status::need_input) {
                                    closeConnection(connection, kProtocolFailureCode);
                                    pushStream->status = Http3ServerConnection::PushStreamOpenResult::Status::kStopped;
                                }
                            }
                        } catch (...) {
                            closeConnection(connection, kProtocolFailureCode);
                            pushStream->status = Http3ServerConnection::PushStreamOpenResult::Status::kStopped;
                        }
                    }
                }
                if (settlement ==
                    Http3ServerConnectionChannel::IntentSettlement::kExecutedHandoff) {
                    if (intent.token.kind ==
                        Http3ServerConnection::TransportIntentKind::kConnectionClose) {
                        closeConnection(connection,
                            intent.connectionErrorCode.value_or(kProtocolFailureCode));
                    } else if (intent.token.kind == Http3ServerConnection::TransportIntentKind::kStreamReset) {
                        terminateRequestStream(connection, intent.token.id.streamId,
                            static_cast<std::uint64_t>(intent.streamResetErrorCode));
                        if (!intent.token.id.pushId) {
                            stopRequestInput(connection, intent.token.id.streamId);
                        }
                    }
                }
                const auto acknowledged = channel.acknowledgeIntentAfterHandoff(
                    connection.identity, intent.token, settlement, pushStream);
                if (acknowledged == Http3ServerConnectionChannel::Status::kFull) {
                    connection.pendingIntentAck = Connection::PendingIntentSettlement{
                        .token = intent.token,
                        .settlement = settlement,
                        .pushStream = pushStream};
                    break;
                }
                if (acknowledged != Http3ServerConnectionChannel::Status::kPublished) {
                    closeConnection(connection, kProtocolFailureCode);
                    break;
                }
            }
        }

        if (stopping_ && !connection.grantReceived && connection.hasLastIdentity &&
            !connection.networkPublicationsClosed) {
            const auto closed = channel.closeNetworkPublications(connection.identity);
            if (closed == Http3ServerConnectionChannel::Status::kPublished) {
                connection.networkPublicationsClosed = true;
                progress = true;
            } else if (closed != Http3ServerConnectionChannel::Status::kWrongState) {
                std::terminate();
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
            !connection.drainCompleteReceived) {
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

        // Physical transport retirement closes the wire driver's publication
        // gate before waiting for the same-worker handler's WorkerFinalized.
        // The existing handshake orders both protocol halves' final records.
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
            const auto rearmed = channel.rearm();
            if (rearmed == Http3ServerConnectionChannel::Status::kPublished) {
                const auto lastIdentity = connection.identity;
                const bool hasLastIdentity = connection.hasLastIdentity;
                connection = Connection(memory_.resource());
                connection.identity = lastIdentity;
                connection.hasLastIdentity = hasLastIdentity;
                notifyWorker(worker);
                progress = true;
            } else if (rearmed != Http3ServerConnectionChannel::Status::kWrongState) {
                std::terminate();
            }
        }
    }
    return progress;
}

bool http3_worker_runtime::admit(worker_link& worker, std::size_t index) noexcept {
    auto& connection = worker.connections[index];
    auto* transport = wire_.transport();
    if (transport == nullptr || !connection.grantReceived || connection.bindPublished ||
        connection.revokeRequested || stopping_) {
        return false;
    }

    bool progress = false;
    try {
        if (!connection.accepted) {
            if (pendingOffers_.empty()) {
                return false;
            }
            connection.streams.reserve(ruvia::quic_limits{}.max_streams);
            connection.pushStreams.reserve(kHttp3ServerPushAllowance);
            const auto admitted = transport->admit_initial(
                pendingOffers_.front(), std::chrono::steady_clock::now());
            if (admitted.status == ruvia::quic_operation_status::would_block ||
                admitted.status == ruvia::quic_operation_status::need_input) {
                return false;
            }
            pendingOffers_.erase(pendingOffers_.begin());
            if (admitted.status != ruvia::quic_operation_status::accepted) {
                return true;
            }
            connection.transportId = admitted.connection;
            connection.accepted = true;
            connection.handshakeDeadline =
                std::chrono::steady_clock::now() + handshakeTimeout_;
            progress = true;
        }

        auto& quic = transport->server().connection(connection.transportId);
        const auto info = quic.info();
        if (info.state == ruvia::quic_connection_state::failed ||
            info.state == ruvia::quic_connection_state::retired) {
            transport->retire(connection.transportId);
            connection.accepted = false;
            connection.handshakeDeadline.reset();
            return requestRevoke(worker, index) || progress;
        }
        const auto tls_info = quic.tls_handshake().info();
        const bool negotiated_h3 = tls_info.negotiated_alpn.size() == 2 &&
                                   tls_info.negotiated_alpn[0] == std::byte{static_cast<unsigned char>('h')} &&
                                   tls_info.negotiated_alpn[1] == std::byte{static_cast<unsigned char>('3')};
        if (!info.tls_handshake_complete || !info.quic_handshake_complete ||
            !info.confirmed || !negotiated_h3) {
            return progress;
        }
        const auto peer = to_udp_endpoint(from_quic_address(info.peer_address));
        if (!peer) {
            transport->retire(connection.transportId);
            connection.accepted = false;
            connection.handshakeDeadline.reset();
            (void)requestRevoke(worker, index);
            return true;
        }
        connection.remote_address = peer->address().to_string();
        const auto committed = worker.channels[index]->commitAccepted(connection.identity,
            {.remoteAddress = connection.remote_address,
                .clientCertificateSubject = {},
                .remotePort = peer->port()},
            localSettings_, quic.max_datagram_payload_size());
        if (committed != Http3ServerConnectionChannel::Status::kPublished) {
            reportFailure(std::make_exception_ptr(
                std::runtime_error("HTTP/3 accepted connection could not publish its Bind")));
            return progress;
        }
        connection.handshakeDeadline.reset();
        connection.bindPublished = true;
        return true;
    } catch (const ruvia::quic_error&) {
        if (connection.accepted) {
            transport->retire(connection.transportId);
            connection.accepted = false;
        }
        connection.handshakeDeadline.reset();
        return requestRevoke(worker, index) || progress;
    } catch (...) {
        reportFailure(std::current_exception());
        return progress;
    }
}

bool http3_worker_runtime::retireUnbound(worker_link& worker, std::size_t index) noexcept {
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
        transport->retire(connection.transportId);
        connection.accepted = false;
        connection.handshakeDeadline.reset();
        progress = true;
    }
    return requestRevoke(worker, index) || progress;
}

bool http3_worker_runtime::requestRevoke(worker_link& worker, std::size_t index) noexcept {
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

bool http3_worker_runtime::attach(worker_link& worker, std::size_t index) noexcept {
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
        const auto prefixes = Http3LocalCriticalStreams::create({.enableConnectProtocol = true});
        auto* transport = wire_.transport();
        if (!prefixes || transport == nullptr) {
            closeConnection(connection, kProtocolFailureCode);
            return true;
        }
        connection.critical = makePmrObject<Http3CriticalStreamDriver>(
            memory_.resource(), *prefixes);
        auto planner = Http3ServerRequestAdmissionPlanner::create({
            .maxRequestsPerConnection = static_cast<std::uint64_t>(
                worker.target.max_requests_per_connection),
        });
        if (!planner) {
            closeConnection(connection, kProtocolFailureCode);
            return true;
        }
        connection.admissionPlanner.emplace(std::move(*planner));
        auto& quic = transport->server().connection(connection.transportId);
        connection.output = makePmrObject<Http3ServerStreamOutput>(memory_.resource(),
            quic, memory_.resource(), connection.identity.epoch,
            connection.identity.connectionGeneration,
            Http3ServerStreamOutputConfig{
                .maxTrackedStreams = worker.target.max_requests_per_connection,
                .maxQueuedBlocks = worker.target.mailbox_capacity,
                .maxDriveWorkItems = 16,
                .writeTimeout = worker.target.write_timeout,
            });
        connection.attached = true;
    } catch (...) {
        closeConnection(connection, kProtocolFailureCode);
    }
    return true;
}

bool http3_worker_runtime::pumpInput(worker_link& worker, std::size_t index) {
    auto& connection = worker.connections[index];
    auto& transport = *wire_.transport();
    auto& quic = transport.server().connection(connection.transportId);
    bool progress = false;

    const auto critical = connection.critical->drive(
        [&quic](Http3CriticalStreamDriver::Kind) {
            return quic.open_stream(true);
        },
        [&quic](std::uint64_t id, std::span<const char> bytes) {
            return quic.write_stream(id, std::as_bytes(bytes));
        });
    if (critical == Http3CriticalStreamDriver::Result::kFatal) {
        closeConnection(connection, kProtocolFailureCode);
        return true;
    }
    progress = critical == Http3CriticalStreamDriver::Result::kProgress;
    if (connection.goawayQueued && critical == Http3CriticalStreamDriver::Result::kReady) {
        connection.goawayBytesAccepted = true;
    }

    const auto accepted = quic.accept_streams();
    if (accepted.status != ruvia::quic_operation_status::accepted &&
        accepted.status != ruvia::quic_operation_status::need_input &&
        accepted.status != ruvia::quic_operation_status::would_block) {
        closeConnection(connection, kProtocolFailureCode);
        return true;
    }
    for (std::size_t i = 0; i < accepted.size; ++i) {
        const auto streamId = accepted.streams[i].stream_id;
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
            if (connection.admittedRequestCount == worker.target.max_requests_per_connection) {
                if (!announceGoaway(worker, index) || !sealAdmission(worker, index)) {
                    return true;
                }
            }
        }
        progress = true;
    }

    const auto streamCount = connection.streams.size();
    const auto startInputIndex = streamCount == 0
                                     ? std::size_t{0}
                                     : connection.nextInputStreamIndex % streamCount;
    const auto streamTurnBudget = (std::min)(streamCount, kInputStreamPumpBudget);
    std::size_t processedStreams = 0;
    for (; processedStreams < streamTurnBudget; ++processedStreams) {
        const auto streamIndex = (startInputIndex + processedStreams) % streamCount;
        auto& stream = connection.streams[streamIndex];
        // Terminal input can still need one final worker notification (notably
        // write-timeout cancellation after FIN was already delivered).
        if (stream.pendingControl) {
            const auto sent = worker.requestMailbox.trySendControl(*stream.pendingControl);
            if (sent == Http3StreamMailbox::ControlResult::kFull) {
                progress |= worker.requestMailbox.armCapacityWait(Http3StreamMailbox::CapacityInterest::kControl) ==
                            Http3StreamMailbox::CapacityWaitResult::kReady;
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
                                        ? ruvia::quic_operation_status::retired
                                        : streamTransport->server().connection(connection.transportId).close_stream(stream.id);
                if (closed != ruvia::quic_operation_status::accepted &&
                    closed != ruvia::quic_operation_status::completed &&
                    closed != ruvia::quic_operation_status::retired) {
                    closeConnection(connection, kProtocolFailureCode);
                    return true;
                }
            }
            completeInputTerminal(connection, stream.id);
            progress = true;
            continue;
        }
        if (stream.inputTerminal) {
            continue;
        }

        if (stream.requestStream) {
            const auto timeout = stream.receivePhase == Stream::ReceivePhase::kHeaders
                                     ? worker.target.request_header_timeout
                                 : stream.bodyTimeoutApplies()
                                     ? worker.target.request_body_timeout
                                     : std::nullopt;
            if (phaseTimeoutExpired(timeout, stream.lastInputActivity,
                    std::chrono::steady_clock::now())) {
                noteInputReset(connection, stream.id);
                terminateRequestStream(connection, stream.id,
                    static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestCancelled));
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
        stream.receivedEarlyData = stream.receivedEarlyData ||
                                   quic.stream_info(stream.id).received_early_data;
        const Http3StreamMessageId messageId{connection.identity.epoch,
            connection.identity.connectionGeneration, stream.id, {}, stream.receivedEarlyData};
        const auto reserved = worker.requestMailbox.reserveData(messageId, reservation);
        if (reserved == Http3StreamMailbox::ReservationResult::kFull ||
            reserved == Http3StreamMailbox::ReservationResult::kNoBlock) {
            progress |= worker.requestMailbox.armCapacityWait(Http3StreamMailbox::CapacityInterest::kData) ==
                        Http3StreamMailbox::CapacityWaitResult::kReady;
            continue;
        }
        if (reserved != Http3StreamMailbox::ReservationResult::kReserved) {
            closeConnection(connection, kServerShutdownCode);
            return true;
        }
        auto writableBytes = reservation.writableBytes();
        auto writable = std::span<char>(reinterpret_cast<char*>(writableBytes.data()),
            writableBytes.size());
        const auto read = quic.read_stream(stream.id, std::as_writable_bytes(writable));
        switch (read.status) {
            case ruvia::quic_stream_read_status::data: {
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
            case ruvia::quic_stream_read_status::would_block:
                reservation.abort();
                break;
            case ruvia::quic_stream_read_status::fin:
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
                notePeerFin(connection, stream.id);
                stream.pendingControl = Http3StreamControl{
                    .kind = Http3StreamControl::Kind::kStreamFin,
                    .id = messageId,
                    .value = stream.receivedBytes,
                };
                progress = true;
                break;
            case ruvia::quic_stream_read_status::reset:
                reservation.abort();
                noteInputReset(connection, stream.id);
                stream.frameTracker.reset();
                if (stream.requestStream) {
                    terminateRequestStream(connection, stream.id,
                        static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestCancelled));
                }
                stream.pendingControl = Http3StreamControl{
                    .kind = Http3StreamControl::Kind::kStreamReset,
                    .id = messageId,
                    .value = stream.receivedBytes,
                    .streamResetErrorCode = static_cast<Http3ConnectionErrorCode>(
                        read.peer_reset_error_code.value_or(static_cast<std::uint64_t>(
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
    const auto streamCountBeforeRetirement = connection.streams.size();
    std::erase_if(connection.streams, [](const Stream& stream) {
        // Keep request-stream identity through the connection lifetime: its
        // deferred TunnelEstablished marker may be published after peer FIN.
        return stream.inputTerminal && !stream.requestStream;
    });
    if (connection.streams.size() != streamCountBeforeRetirement) {
        connection.nextInputStreamIndex = 0;
        connection.nextTunnelHandshakeStreamIndex = 0;
        connection.tunnelHandshakeScanRemaining = connection.pendingTunnelHandshakes == 0
                                                      ? 0
                                                      : connection.streams.size();
        connection.tunnelHandshakeScanDirty = false;
    } else if (streamCount != 0) {
        connection.nextInputStreamIndex =
            (startInputIndex + processedStreams) % streamCount;
    }
    return progress || (processedStreams == streamTurnBudget &&
                           streamTurnBudget < streamCount);
}

void http3_worker_runtime::terminateRequestStream(Connection& connection,
    std::uint64_t streamId, std::uint64_t errorCode) {
    bool terminateDirectly = connection.output == nullptr;
    if (connection.output != nullptr) {
        const auto beforeCancel = connection.output->streamInfo(streamId);
        if (beforeCancel && beforeCancel->sendFinAccepted) {
            // cancelStream closes only the receive side when the real local FIN
            // has already been accepted by QUIC. Never fall back to RESET_STREAM
            // based solely on the HTTP response's terminal state.
            const auto result = connection.output->cancelStream(streamId, errorCode);
            switch (result.status) {
                case Http3ServerStreamOutput::Status::kCancelled:
                case Http3ServerStreamOutput::Status::kClosedStream:
                case Http3ServerStreamOutput::Status::kConnectionClosed:
                case Http3ServerStreamOutput::Status::kStopped:
                    return;
                default:
                    closeConnection(connection, kProtocolFailureCode);
                    return;
            }
        }
        const auto result = connection.output->cancelStream(streamId, errorCode);
        switch (result.status) {
            case Http3ServerStreamOutput::Status::kCancelled:
            case Http3ServerStreamOutput::Status::kConnectionClosed:
            case Http3ServerStreamOutput::Status::kStopped:
                return;
            case Http3ServerStreamOutput::Status::kClosedStream: {
                const auto info = connection.output->streamInfo(streamId);
                if (!info || info->state == Http3ServerStreamOutput::StreamState::kFinished) {
                    terminateDirectly = true;
                } else {
                    return;
                }
                break;
            }
            default:
                closeConnection(connection, kProtocolFailureCode);
                return;
        }
    }
    if (!terminateDirectly) {
        return;
    }
    auto* transport = wire_.transport();
    if (transport == nullptr) {
        closeConnection(connection, kProtocolFailureCode);
        return;
    }
    try {
        const auto status = transport->server().connection(connection.transportId).terminate_bidirectional_stream(streamId, errorCode);
        if (status != ruvia::quic_operation_status::accepted &&
            status != ruvia::quic_operation_status::completed &&
            status != ruvia::quic_operation_status::retired) {
            closeConnection(connection, kProtocolFailureCode);
        }
    } catch (...) {
        closeConnection(connection, kProtocolFailureCode);
    }
}

http3_worker_runtime::TunnelEstablishedResult
http3_worker_runtime::acceptTunnelEstablished(Connection& connection,
    const Http3StreamControl& control, std::uint64_t acceptedWireBytes) noexcept {
    const auto found = std::ranges::find_if(connection.streams,
        [&control](const Stream& stream) { return stream.id == control.id.streamId; });
    if (found == connection.streams.end()) {
        return TunnelEstablishedResult::kProtocolFailure;
    }
    const auto result =
        found->acceptTunnelEstablished(control, connection.identity, acceptedWireBytes);
    if (result != TunnelEstablishedResult::kAccepted) {
        return result;
    }
    if (found->tunnelEstablishedBarrier) {
        if (connection.pendingTunnelHandshakes ==
            std::numeric_limits<std::size_t>::max()) {
            std::terminate();
        }
        ++connection.pendingTunnelHandshakes;
        if (connection.tunnelHandshakeScanRemaining == 0) {
            connection.tunnelHandshakeScanRemaining = connection.streams.size();
            connection.nextTunnelHandshakeStreamIndex = 0;
            connection.tunnelHandshakeScanDirty = false;
        } else if (connection.tunnelHandshakeScanRemaining < connection.streams.size()) {
            connection.tunnelHandshakeScanDirty = true;
        }
    }
    return TunnelEstablishedResult::kAccepted;
}

bool http3_worker_runtime::confirmTunnelEstablished(Connection& connection,
    std::uint64_t streamId,
    std::uint64_t acceptedWireBytes) noexcept {
    const auto found = std::ranges::find_if(connection.streams,
        [streamId](const Stream& stream) { return stream.id == streamId; });
    if (found == connection.streams.end() ||
        !found->confirmTunnelEstablished(acceptedWireBytes)) {
        return false;
    }
    if (connection.pendingTunnelHandshakes == 0) {
        std::terminate();
    }
    --connection.pendingTunnelHandshakes;
    if (connection.pendingTunnelHandshakes == 0) {
        connection.tunnelHandshakeScanRemaining = 0;
        connection.tunnelHandshakeScanDirty = false;
    }
    return true;
}

void http3_worker_runtime::notePeerFin(Connection& connection,
    std::uint64_t streamId) noexcept {
    const auto found = std::ranges::find_if(connection.streams,
        [streamId](const Stream& stream) { return stream.id == streamId; });
    if (found != connection.streams.end()) {
        found->inputFin = true;
    }
}

void http3_worker_runtime::noteInputReset(Connection& connection,
    std::uint64_t streamId) noexcept {
    const auto found = std::ranges::find_if(connection.streams,
        [streamId](const Stream& stream) { return stream.id == streamId; });
    if (found == connection.streams.end()) {
        return;
    }
    found->inputReset = true;
    found->frameTracker.reset();
    if (found->tunnelEstablishedBarrier) {
        if (connection.pendingTunnelHandshakes == 0) {
            std::terminate();
        }
        --connection.pendingTunnelHandshakes;
        found->tunnelEstablishedBarrier.reset();
    }
    if (connection.pendingTunnelHandshakes == 0) {
        connection.tunnelHandshakeScanRemaining = 0;
        connection.tunnelHandshakeScanDirty = false;
    }
}

void http3_worker_runtime::completeInputTerminal(Connection& connection,
    std::uint64_t streamId) noexcept {
    const auto found = std::ranges::find_if(connection.streams,
        [streamId](const Stream& stream) { return stream.id == streamId; });
    if (found == connection.streams.end()) {
        return;
    }
    if (found->inputReset && found->tunnelEstablishedBarrier) {
        if (connection.pendingTunnelHandshakes == 0) {
            std::terminate();
        }
        --connection.pendingTunnelHandshakes;
        found->tunnelEstablishedBarrier.reset();
    }
    found->inputTerminal = true;
}

void http3_worker_runtime::stopRequestInput(Connection& connection,
    std::uint64_t streamId) noexcept {
    const auto found = std::ranges::find_if(connection.streams,
        [streamId](const Stream& stream) { return stream.id == streamId; });
    if (found == connection.streams.end()) {
        return;
    }
    found->pendingControl.reset();
    noteInputReset(connection, streamId);
    completeInputTerminal(connection, streamId);
}

bool http3_worker_runtime::announceGoaway(worker_link& worker, std::size_t index) noexcept {
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

bool http3_worker_runtime::sealAdmission(worker_link& worker, std::size_t index) noexcept {
    auto& connection = worker.connections[index];
    if (connection.admissionSealedPublished) {
        return true;
    }
    if (!connection.goawayQueued || !connection.admissionPlanner ||
        connection.admittedRequestCount != worker.target.max_requests_per_connection) {
        closeConnection(connection, kProtocolFailureCode);
        return false;
    }
    const auto sealed = worker.channels[index]->publishAdmissionSealed(connection.identity,
        worker.target.max_requests_per_connection, connection.admissionPlanner->goawayId());
    if (sealed != Http3ServerConnectionChannel::Status::kPublished) {
        closeConnection(connection, kProtocolFailureCode);
        return false;
    }
    connection.admissionSealedPublished = true;
    notifyWorker(worker);
    return true;
}

bool http3_worker_runtime::rejectRequestStream(worker_link& worker, std::size_t index,
    std::uint64_t streamId) {
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
    const auto status = transport->server().connection(connection.transportId).terminate_bidirectional_stream(streamId, static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestRejected));
    if (status != ruvia::quic_operation_status::accepted &&
        status != ruvia::quic_operation_status::completed &&
        status != ruvia::quic_operation_status::retired) {
        closeConnection(connection, kProtocolFailureCode);
        return false;
    }
    ++connection.rejectedRequestCount;
    return true;
}

bool http3_worker_runtime::pump_datagrams(worker_link& worker, std::size_t index) {
    auto& connection = worker.connections[index];
    auto& quic = wire_.transport()->server().connection(connection.transportId);
    auto& channel = *worker.channels[index];
    bool progress{};
    std::array<std::byte, Http3ServerConnectionChannel::kMaxDatagramBytes> input{};
    for (std::size_t count = 0; count < Http3ServerConnectionChannel::kDatagramCapacity; ++count) {
        const auto result = quic.read_datagram(input);
        if (result.status == ruvia::quic_datagram_status::would_block ||
            result.status == ruvia::quic_datagram_status::unavailable) {
            break;
        }
        progress = true;
        if (result.status != ruvia::quic_datagram_status::received) {
            continue;
        }
        const auto bytes = std::span<const std::byte>(input).first(result.size);
        const auto decoded = decodeHttp3Datagram({reinterpret_cast<const char*>(bytes.data()), bytes.size()});
        if (!decoded) {
            closeConnection(connection, static_cast<Http3ConnectionErrorCode>(kHttp3DatagramErrorCode));
            break;
        }
        const auto stream = std::ranges::find(connection.streams, decoded->streamId, &Stream::id);
        const auto planned = planHttp3DatagramReceive(*decoded,
            {.localH3Datagram = localSettings_.h3Datagram,
                .streamExists = stream != connection.streams.end(),
                .receiveOpen = stream != connection.streams.end() && !stream->inputTerminal && !stream->inputReset,
                .supportsDatagrams = stream != connection.streams.end() && stream->tunnelEstablished});
        if (planned == Http3DatagramReceiveStatus::kConnectionError) {
            closeConnection(connection, static_cast<Http3ConnectionErrorCode>(kHttp3DatagramErrorCode));
            break;
        }
        if (planned == Http3DatagramReceiveStatus::kDeliver) {
            (void)channel.publishRequestDatagram(connection.identity, decoded->streamId, bytes);
        }
    }
    for (std::size_t count = 0; count < Http3ServerConnectionChannel::kDatagramCapacity; ++count) {
        Http3ServerConnectionChannel::Datagram output;
        if (channel.receiveResponseDatagram(output) != Http3ServerConnectionChannel::Status::kReceived) {
            break;
        }
        progress = true;
        if (output.identity.epoch != connection.identity.epoch ||
            output.identity.connectionGeneration != connection.identity.connectionGeneration) {
            continue;
        }
        (void)quic.write_datagram(std::span<const std::byte>(output.bytes).first(output.size));
    }
    return progress;
}

bool http3_worker_runtime::pumpResponses(worker_link& worker) noexcept {
    bool progress = false;
    worker.responseMailboxDrained = false;
    std::size_t processed = 0;
    for (;;) {
        if (worker.pendingResponseControl) {
            auto* connection = findConnection(worker, worker.pendingResponseControl->id);
            if (connection == nullptr || connection->output == nullptr) {
                worker.pendingResponseControl.reset();
                ++processed;
                progress = true;
            } else {
                if (worker.pendingResponseControl->kind ==
                    Http3StreamControl::Kind::kTunnelEstablished) {
                    // Input (including peer reset/timeout) is observed before the
                    // worker control lane; a terminal same-stream marker can lag it.
                    const auto info = connection->output->streamInfo(
                        worker.pendingResponseControl->id.streamId);
                    switch (acceptTunnelEstablished(*connection,
                        *worker.pendingResponseControl,
                        info ? info->acceptedWireBytes : 0)) {
                        case TunnelEstablishedResult::kAccepted:
                        case TunnelEstablishedResult::kIgnoredTerminal:
                            break;
                        case TunnelEstablishedResult::kProtocolFailure:
                            closeConnection(*connection, kProtocolFailureCode);
                            break;
                    }
                    worker.pendingResponseControl.reset();
                    ++processed;
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
                    ++processed;
                    progress = true;
                }
            }
        }
        if (processed >= kResponseMailboxPumpBudget) {
            break;
        }
        if (worker.pendingResponse) {
            const auto* critical = worker.pendingResponse->critical();
            auto* connection = critical
                                   ? findConnection(worker, critical->epoch, critical->connectionGeneration)
                                   : findConnection(worker, worker.pendingResponse->id());
            if (connection == nullptr || connection->output == nullptr) {
                worker.pendingResponse->release();
                worker.pendingResponse.reset();
                ++processed;
                progress = true;
            } else {
                const auto critical_stream = critical && connection->critical
                                                 ? connection->critical->streamId(critical->kind)
                                                 : std::nullopt;
                if (critical && !critical_stream) {
                    break;  // The local critical stream must be opened before output handoff.
                }
                const auto result = critical
                                        ? connection->output->acceptCriticalData(*worker.pendingResponse, *critical_stream)
                                        : connection->output->acceptData(*worker.pendingResponse);
                if (result.status == Http3ServerStreamOutput::Status::kBackpressured) {
                    break;
                }
                if (result.status != Http3ServerStreamOutput::Status::kAccepted &&
                    result.status != Http3ServerStreamOutput::Status::kClosedStream) {
                    worker.pendingResponse->release();
                    closeConnection(*connection, kProtocolFailureCode);
                }
                worker.pendingResponse.reset();
                ++processed;
                progress = true;
            }
        }
        if (processed >= kResponseMailboxPumpBudget) {
            // Leave remaining mailbox entries for the next protocol turn; the
            // outer worker pump schedules a bounded continuation while work remains.
            break;
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

bool http3_worker_runtime::pumpOutput(worker_link& worker, std::size_t index) {
    auto& connection = worker.connections[index];
    if (connection.output == nullptr) {
        return false;
    }
    if (transportActivityForPump_) {
        connection.output->notifyTransportActivity();
    }
    const auto result = connection.output->drive();
    if (result.status == Http3ServerStreamOutput::Status::kTransportError ||
        result.status == Http3ServerStreamOutput::Status::kFinalSizeError ||
        result.status == Http3ServerStreamOutput::Status::kCapacityExhausted ||
        result.status == Http3ServerStreamOutput::Status::kUnsafeToRelease) {
        closeConnection(connection, kProtocolFailureCode);
    }
    for (auto& stream : connection.streams) {
        if (!stream.requestStream || stream.inputReset || stream.writeTimeoutNotified) {
            continue;
        }
        const auto info = connection.output->streamInfo(stream.id);
        if (info && info->timedOut) {
            // Reset takes precedence over a queued FIN; if FIN was already
            // delivered, inputTerminal does not suppress this cancellation.
            stream.writeTimeoutNotified = true;
            noteInputReset(connection, stream.id);
            stream.frameTracker.reset();
            stream.pendingControl = Http3StreamControl{
                .kind = Http3StreamControl::Kind::kStreamReset,
                .id = {connection.identity.epoch,
                    connection.identity.connectionGeneration, stream.id},
                .value = stream.receivedBytes,
                .streamResetErrorCode = Http3ConnectionErrorCode::kRequestCancelled,
            };
        }
    }
    if (result.acceptedBytes != 0 && connection.pendingTunnelHandshakes != 0) {
        if (connection.tunnelHandshakeScanRemaining == 0) {
            connection.tunnelHandshakeScanRemaining = connection.streams.size();
            connection.nextTunnelHandshakeStreamIndex = 0;
            connection.tunnelHandshakeScanDirty = false;
        } else if (connection.tunnelHandshakeScanRemaining < connection.streams.size()) {
            connection.tunnelHandshakeScanDirty = true;
        }
    }

    bool handshakeProgress = false;
    const auto streamCount = connection.streams.size();
    if (connection.pendingTunnelHandshakes != 0 &&
        connection.tunnelHandshakeScanRemaining != 0) {
        if (streamCount == 0) {
            std::terminate();
        }
        const auto scanBudget = (std::min)({streamCount,
            connection.tunnelHandshakeScanRemaining, kInputStreamPumpBudget});
        for (std::size_t scanned = 0; scanned < scanBudget; ++scanned) {
            const auto streamIndex = connection.nextTunnelHandshakeStreamIndex % streamCount;
            connection.nextTunnelHandshakeStreamIndex = (streamIndex + 1) % streamCount;
            --connection.tunnelHandshakeScanRemaining;
            auto& stream = connection.streams[streamIndex];
            if (!stream.tunnelEstablishedBarrier) {
                continue;
            }
            const auto info = connection.output->streamInfo(stream.id);
            if (info && confirmTunnelEstablished(connection, stream.id,
                            info->acceptedWireBytes)) {
                handshakeProgress = true;
            }
        }
        if (connection.pendingTunnelHandshakes == 0) {
            connection.tunnelHandshakeScanRemaining = 0;
            connection.tunnelHandshakeScanDirty = false;
        } else if (connection.tunnelHandshakeScanRemaining == 0 &&
                   connection.tunnelHandshakeScanDirty) {
            connection.tunnelHandshakeScanRemaining = streamCount;
            connection.nextTunnelHandshakeStreamIndex = 0;
            connection.tunnelHandshakeScanDirty = false;
        }
    }
    return result.madeProgress || result.needsReschedule || handshakeProgress ||
           connection.tunnelHandshakeScanRemaining != 0;
}

bool http3_worker_runtime::retire(worker_link& worker, std::size_t index) {
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
    auto& quic = transport->server().connection(connection.transportId);
    bool forceLocalRetirement = stopping_ || connection.gracefulCloseAbandoned ||
                                (connection.closeStarted && connection.drainDeadline &&
                                    std::chrono::steady_clock::now() >= *connection.drainDeadline);
    const auto state = quic.info().state;
    if (state == ruvia::quic_connection_state::failed) {
        forceLocalRetirement = true;
    }
    if (state != ruvia::quic_connection_state::retired &&
        state != ruvia::quic_connection_state::closing &&
        state != ruvia::quic_connection_state::draining) {
        static constexpr std::string_view graceful_reason = "HTTP/3 drain complete";
        static constexpr std::string_view close_reason = "HTTP/3 connection closed";
        const auto reason = connection.gracefulCloseStarted ? graceful_reason : close_reason;
        try {
            const auto close = quic.close({
                .kind = ruvia::quic_close_kind::application,
                .code = static_cast<std::uint64_t>(
                    connection.closeErrorCode.value_or(kServerShutdownCode)),
                .reason = {reason.data(), reason.size()},
            });
            if (close == ruvia::quic_operation_status::would_block ||
                close == ruvia::quic_operation_status::need_input) {
                if (!forceLocalRetirement) {
                    return false;
                }
            }
            if (close != ruvia::quic_operation_status::accepted &&
                close != ruvia::quic_operation_status::completed &&
                close != ruvia::quic_operation_status::closing &&
                close != ruvia::quic_operation_status::draining &&
                close != ruvia::quic_operation_status::retired) {
                connection.gracefulCloseAbandoned = true;
                forceLocalRetirement = true;
            }
        } catch (const ruvia::quic_error&) {
            connection.gracefulCloseAbandoned = true;
            forceLocalRetirement = true;
        }
    }
    if (!forceLocalRetirement &&
        quic.info().state != ruvia::quic_connection_state::retired) {
        return false;
    }

    // Stop the stream writer before retiring its connection so all borrowed
    // mailbox blocks are returned only after Core accepts or invalidates writes.
    if (connection.output != nullptr) {
        const auto stopped = connection.output->stop();
        if (stopped.status == Http3ServerStreamOutput::Status::kUnsafeToRelease) {
            return false;
        }
        connection.output.reset();
    }
    connection.critical.reset();
    transport->retire(connection.transportId);
    connection.accepted = false;
    connection.handshakeDeadline.reset();
    if (worker.channels[index]->publishTransportRetired(connection.identity) !=
        Http3ServerConnectionChannel::Status::kPublished) {
        std::terminate();
    }
    connection.transportRetiredPublished = true;
    notifyWorker(worker);
    return true;
}

http3_worker_runtime::Connection* http3_worker_runtime::findConnection(
    worker_link& worker, Http3StreamMessageId id) noexcept {
    return findConnection(worker, id.epoch, id.connectionGeneration);
}

http3_worker_runtime::Connection* http3_worker_runtime::findConnection(
    worker_link& worker, std::uint64_t epoch, std::uint64_t generation) noexcept {
    const auto found = std::ranges::find_if(worker.connections, [epoch, generation](const Connection& value) {
        return value.attached && value.identity.epoch == epoch &&
               value.identity.connectionGeneration == generation;
    });
    return found == worker.connections.end() ? nullptr : &*found;
}

void http3_worker_runtime::closeConnection(Connection& connection,
    Http3ConnectionErrorCode reason) noexcept {
    if (!connection.accepted || connection.closeStarted) {
        return;
    }
    if (connection.gracefulCloseStarted) {
        connection.gracefulCloseAbandoned = true;
        return;
    }
    connection.closeStarted = true;
    connection.closeErrorCode = reason;
    if (!connection.drainDeadline) {
        connection.drainDeadline = std::chrono::steady_clock::now() + drainTimeout_;
    }
    if (auto* transport = wire_.transport(); transport != nullptr) {
        static constexpr std::string_view close_reason = "HTTP/3 connection closing";
        try {
            (void)transport->server().connection(connection.transportId).close({
                .kind = ruvia::quic_close_kind::application,
                .code = static_cast<std::uint64_t>(reason),
                .reason = {close_reason.data(), close_reason.size()},
            });
        } catch (...) {
            connection.gracefulCloseAbandoned = true;
        }
    }
}

void http3_worker_runtime::notifyWorker(worker_link& worker) noexcept {
    (void)worker.target.server->notification().notify();
}

}  // namespace ruvia::detail
