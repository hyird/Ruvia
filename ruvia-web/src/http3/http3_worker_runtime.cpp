#include "http3/http3_worker_runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>

#include <asio/bind_allocator.hpp>
#include <asio/post.hpp>

#include "ruvia/core/Async.h"
#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/http/Http3PeerStreams.h"
#include "ruvia/http/Http3StreamFrames.h"

#include "http3/Http3QuicSocketAddress.h"
#include "http3/http3_worker_server.h"
#include "server/HttpServerListener.h"
#include "server/HttpServerOptionsValidation.h"

namespace ruvia::detail {
namespace {

constexpr auto kMonitorInterval = std::chrono::milliseconds(10);
constexpr std::size_t kResponseBufferPumpBudget = 64;

std::size_t connection_capacity(const http3_worker_runtime::worker_target& worker) {
    if (worker.server == nullptr || worker.max_connections == 0 ||
        worker.buffer_capacity == 0 || worker.max_requests_per_connection == 0) {
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

http3_worker_datagram_endpoint::pump_result send_http3_version_negotiation(
    ruvia::quic_server& server, ruvia::quic_version_negotiation_plan& plan,
    http3_worker_datagram_endpoint& endpoint) {
    const auto packet_buffer = endpoint.packet_buffer();
    if (packet_buffer.empty()) {
        return http3_worker_datagram_endpoint::pump_result::idle;
    }
    struct reservation_guard final {
        http3_worker_datagram_endpoint& endpoint;
        ~reservation_guard() {
            endpoint.cancel_packet();
        }
    } guard{endpoint};
    const auto packet = server.write_version_negotiation(plan, packet_buffer);
    if (packet.status == ruvia::quic_operation_status::would_block) {
        return http3_worker_datagram_endpoint::pump_result::idle;
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
    if (sent == http3_worker_datagram_endpoint::pump_result::error) {
        throw std::system_error(endpoint.error(), "send HTTP/3 Version Negotiation packet");
    }
    return sent;
}

http3_worker_runtime::worker_link::worker_link(std::pmr::memory_resource* resource,
    http3_worker_runtime& runtime, worker_target configured)
    : target(std::move(configured)),
      request_buffer(target.buffer_capacity, target.buffer_capacity, target.buffer_capacity,
          resource, {.ready = {target.server, [](void* context, std::uint8_t) noexcept {
                                   static_cast<http3_worker_server*>(context)->wake();
                               }},
                        .capacity = {&runtime, [](void* context, std::uint8_t) noexcept {
                                         static_cast<http3_worker_runtime*>(context)->wake();
                                     }}}),
      states(resource),
      state_views(resource),
      connections(resource) {
    states.reserve(target.max_connections);
    state_views.reserve(target.max_connections);
    connections.reserve(target.max_connections);
    for (std::size_t i = 0; i < target.max_connections; ++i) {
        states.push_back(makePmrObject<http3_connection_state>(resource,
            http3_connection_state::local_change_callback{&runtime, [](void* context) noexcept {
                                                              auto& owner = *static_cast<http3_worker_runtime*>(context);
                                                              owner.wake();
                                                              owner.worker_->target.server->wake();
                                                          }},
            resource));
        state_views.push_back(states.back().get());
        connections.emplace_back(resource, *states.back(), request_buffer, runtime.wire_,
            http3_connection_driver_config{
                .max_requests_per_connection = target.max_requests_per_connection,
                .buffer_capacity = target.buffer_capacity,
                .request_header_timeout = target.request_header_timeout,
                .request_body_timeout = target.request_body_timeout,
                .write_timeout = target.write_timeout,
                .drain_timeout = runtime.drainTimeout_,
                .handshake_timeout = runtime.handshakeTimeout_,
                .local_settings = runtime.localSettings_});
        connections.back().install_executor();
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
      protocol_signal_(runtime.handle()),
      notification_tasks_(runtime.handle(), {.resource = memory_.resource()}),
      bindAddress_(local.address()),
      tls_(tls_config, memory_.resource()),
      wire_(ioContext_, datagrams, local, tls_, transport_config(worker, partition),
          memory_.resource(), {this, [](void* context, http3_quic_server_transport& transport, http3_worker_datagram_endpoint& endpoint) noexcept {
                                   auto& self = *static_cast<http3_worker_runtime*>(context);
                                   const bool progress = self.pump_protocol(&transport, &endpoint);
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
    require_owner_thread();
    if (running_ || monitorScheduled_ || !protocol_drained() || datagrams_ != nullptr) {
        std::terminate();
    }
}

void http3_worker_runtime::stage() {
    require_owner_thread();
    if (staged_) {
        throw std::logic_error("HTTP/3 worker runtime already staged");
    }
    auto& worker = *worker_;
    if (!worker.target.server->stage_install({.request_buffer = &worker.request_buffer,
            .connections = worker.state_views,
            .protocol_ready = {this, [](void* context, std::uint8_t) noexcept {
                                   static_cast<http3_worker_runtime*>(context)->wake();
                               }}})) {
        throw std::runtime_error("failed to stage HTTP/3 worker link");
    }
    staged_ = true;
}

void http3_worker_runtime::start() {
    require_owner_thread();
    if (!staged_ || running_ || stopping_) {
        throw std::logic_error("HTTP/3 worker runtime cannot start in this state");
    }
    running_ = true;
    wire_.start();
    wire_.requestDrive();
    schedule_monitor();
}

void http3_worker_runtime::wake() noexcept {
    require_owner_thread();
    protocol_signal_.notify();
}

void http3_worker_runtime::stop() noexcept {
    require_owner_thread();
    if (datagrams_ == nullptr) {
        return;  // finish_datagrams already released every channel dependency.
    }
    const bool alreadyStopping = stopping_;
    request_stop_on_owner();
    if (!alreadyStopping) {
        (void)pump_protocol();
    }
    wire_.pollStop();
    if (!protocol_drained()) {
        schedule_monitor();
    }
    wake();
}

bool http3_worker_runtime::protocol_drained() const noexcept {
    require_owner_thread();
    if (datagrams_ == nullptr) {
        return true;
    }
    if (!stopping_) {
        return false;
    }
    const bool states_done = std::ranges::all_of(worker_->states,
        [](const auto& channel) { return channel->ready_to_destroy(); });
    return states_done && worker_->target.server->drained() &&
           wire_.stopStatus().complete() && !monitorScheduled_;
}

bool http3_worker_runtime::drained() const noexcept {
    return protocol_drained() && datagrams_ == nullptr &&
           (!datagram_run_started_ || datagram_run_retired_);
}

asio::ip::udp::endpoint http3_worker_runtime::local_endpoint() const {
    require_owner_thread();
    return {bindAddress_, wire_.boundPort()};
}

Task<void> http3_worker_runtime::join() {
    require_owner_thread();
    while (!drained()) {
        co_await protocol_signal_.wait();
    }
}

Task<void> http3_worker_runtime::run_notifications() {
    // This is the worker's only native datagram wait. Core registers the wait
    // and rechecks its pending latch; local protocol work uses WorkerSignal.
    try {
        while (!native_stopping_) {
            const auto status = co_await datagrams_->worker_notification().wait();
            if (native_stopping_) {
                break;
            }
            wire_.poll_datagrams();
            if (datagrams_->acceptor_closed()) {
                request_stop_on_owner();
            }
            if (status == WorkerNotificationWaitStatus::kClosed) {
                abandon_requested_ = true;
                stop();
                break;
            }
            wake();
        }
    } catch (...) {
        report_failure(std::current_exception());
    }
    wake();
}

Task<void> http3_worker_runtime::run_datagrams() {
    require_owner_thread();
    if (datagrams_ == nullptr || datagram_run_started_) {
        std::terminate();
    }
    datagram_run_started_ = true;
    std::size_t inline_turns{};
    const auto post_continuation = [this](auto completion) {
        asio::post(ioContext_.get_executor(), asio::bind_allocator(
                                                  std::pmr::polymorphic_allocator<std::byte>(memory_.resource()),
                                                  [completion = std::move(completion)]() mutable {
                                                      completion(asio::error_code{});
                                                  }));
    };
    try {
        notification_tasks_.spawn(run_notifications());
        while (!protocol_drained()) {
            if (datagrams_->acceptor_closed()) {
                request_stop_on_owner();
            }
            wire_.poll_datagrams();
            if (stopping_) {
                (void)pump_protocol();
                wire_.pollStop();
            } else {
                wire_.requestDrive();
            }
            if (!protocol_drained()) {
                if (++inline_turns == 16) {
                    inline_turns = 0;
                    const auto yielded = co_await ruvia::asyncAsio(post_continuation);
                    if (yielded.errorCode()) {
                        throw std::system_error(yielded.errorCode(), "yield HTTP/3 protocol worker");
                    }
                }
                co_await protocol_signal_.wait();
            }
        }
    } catch (...) {
        report_failure(std::current_exception());
    }
    while (!protocol_drained()) {
        wire_.poll_datagrams();
        (void)pump_protocol();
        wire_.pollStop();
        if (!protocol_drained()) {
            if (++inline_turns == 16) {
                inline_turns = 0;
                const auto yielded = co_await ruvia::asyncAsio(post_continuation);
                if (yielded.errorCode()) {
                    report_failure(std::make_exception_ptr(std::system_error(
                        yielded.errorCode(), "yield retiring HTTP/3 protocol worker")));
                }
            }
            co_await protocol_signal_.wait();
        }
    }
    native_stopping_ = true;
    datagrams_->worker_notification().close();
    try {
        co_await notification_tasks_.join();
    } catch (...) {
        report_failure(std::current_exception());
    }
    datagram_run_retired_ = true;
    finish_datagrams();
}

void http3_worker_runtime::abandon_before_launch() noexcept {
    require_owner_thread();
    if (datagram_run_started_) {
        std::terminate();
    }
    if (datagrams_ == nullptr) {
        return;
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
    if (!worker_->request_buffer.quiescent() ||
        !worker_->target.server->response_buffer().quiescent()) {
        std::terminate();
    }
    worker_->request_buffer.set_local_notifications({});
    worker_->target.server->response_buffer().set_local_notifications({});
    // wire stop has detached its endpoint; no borrowed channel survives ACK.
    auto* channel = std::exchange(datagrams_, nullptr);
    channel->worker_close();
}

void http3_worker_runtime::require_owner_thread() const noexcept {
    if (std::this_thread::get_id() != ownerThread_) {
        std::terminate();
    }
}

void http3_worker_runtime::request_stop_on_owner() noexcept {
    require_owner_thread();
    if (stopping_) {
        return;
    }
    stopping_ = true;
    pendingOffers_.clear();
    for (auto& state : worker_->states) {
        state->stop_admission();
    }
    worker_->target.server->request_stop();
    for (auto& connection : worker_->connections) {
        connection.request_stop();
    }
    wake();
}

void http3_worker_runtime::report_failure(std::exception_ptr failure) noexcept {
    require_owner_thread();
    if (failure_ == nullptr) {
        failure_ = failure;
    }
    request_stop_on_owner();
    if (failureReported_) {
        return;
    }
    failureReported_ = true;
    if (failureNotification_.notify != nullptr) {
        failureNotification_.notify(failureNotification_.context, failure_);
    }
    schedule_monitor();
}

void http3_worker_runtime::schedule_monitor() noexcept {
    require_owner_thread();
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
        report_failure(std::current_exception());
        if (!protocol_drained()) {
            // A failed monitor initiation must not leave joined owners waiting
            // on a wire callback that can no longer make ordered progress.
            std::terminate();
        }
    }
}

void http3_worker_runtime::monitor(const asio::error_code& error) noexcept {
    require_owner_thread();
    monitorScheduled_ = false;
    if (error) {
        if (error != asio::error::operation_aborted) {
            try {
                throw std::system_error(error, "HTTP/3 worker monitor timer");
            } catch (...) {
                report_failure(std::current_exception());
            }
        }
    } else {
        if (const auto failure = wire_.failure()) {
            report_failure(failure);
        }
        if (stopping_) {
            (void)pump_protocol();
            wire_.pollStop();
        } else if (running_) {
            wire_.requestDrive();
        }
    }
    if (!protocol_drained()) {
        schedule_monitor();
    }
    if (abandon_requested_) {
        finish_datagrams();
    }
    wake();
}

bool http3_worker_runtime::pump_protocol(http3_quic_server_transport* transport,
    http3_worker_datagram_endpoint* endpoint) noexcept {
    require_owner_thread();
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
                                route.version_negotiation, *endpoint) ==
                            http3_worker_datagram_endpoint::pump_result::pending) {
                            anyProgress = true;
                        }
                    } else if (route.kind == ruvia::quic_server_route_kind::existing_connection) {
                        const ruvia::quic_datagram_view datagram{
                            received->bytes, to_quic_address(*local), to_quic_address(*peer)};
                        try {
                            (void)transport->server().receive(route.connection, datagram, now);
                        } catch (const ruvia::quic_error&) {
                            for (auto& connection : worker->connections) {
                                if (connection.transport_token() == route.connection) {
                                    connection.transport_failure();
                                }
                            }
                        }
                    }
                }
                const auto consumed = endpoint->consume_receive();
                if (consumed == http3_worker_datagram_endpoint::pump_result::error) {
                    throw std::system_error(endpoint->error(), "consume HTTP/3 UDP receive slot");
                }
                anyProgress = true;
                if (endpoint->receive_slot()) {
                    // Preserve a local continuation when the wire turn exhausts
                    // its budget; another UDP/native edge is not required.
                    wake();
                }
            }
            for (auto& connection : worker->connections) {
                connection.observe_transport(now);
            }
        }
        for (std::size_t pass = 0; pass < pumpBudget_; ++pass) {
            const bool progress = pump_worker(*worker_);
            // One observed edge wakes every connection in the first pass only.
            // Reusing it during local retries restarts blocked output scans
            // without new ACK/credit, delaying the packet flush below.
            transportActivityForPump_ = false;
            if (!progress) {
                exhaustedBudget = false;
                break;
            }
            anyProgress = true;
        }

        if (!stopping_ && transport != nullptr && endpoint != nullptr) {
            const auto count = worker_->connections.size();
            const auto budget = (std::min)(count, std::size_t{16});
            for (std::size_t attempt = 0; attempt < budget; ++attempt) {
                const auto index = next_packet_connection_++ % count;
                auto& connection = worker_->connections[index];
                if (!connection.transport_token()) {
                    continue;
                }
                const auto packet_buffer = endpoint->packet_buffer();
                if (packet_buffer.empty()) {
                    break;  // Pause production only. RX/expiry/local pump ran above.
                }
                struct reservation_guard final {
                    http3_worker_datagram_endpoint& endpoint;
                    ~reservation_guard() {
                        endpoint.cancel_packet();
                    }
                } guard{*endpoint};
                try {
                    const auto packet = connection.write_packet(packet_buffer,
                        std::chrono::steady_clock::now());
                    if (packet.size == 0) {
                        continue;
                    }
                    const auto source = to_udp_endpoint(from_quic_address(packet.local));
                    const auto peer = to_udp_endpoint(from_quic_address(packet.peer));
                    if (!source || !peer || packet.size > packet_buffer.size()) {
                        connection.transport_failure();
                        continue;
                    }
                    const auto sent = endpoint->send_datagram(
                        std::span<const std::byte>(packet_buffer).first(packet.size), *source, *peer);
                    if (sent == http3_worker_datagram_endpoint::pump_result::error) {
                        throw std::system_error(endpoint->error(), "send HTTP/3 QUIC packet");
                    }
                    anyProgress = true;
                    // Further production takes another cooperative local turn.
                    wake();
                } catch (const ruvia::quic_error&) {
                    connection.transport_failure();
                }
            }
        }

        if (stopping_) {
            const bool statesDone = std::ranges::all_of(worker_->states,
                [](const auto& channel) { return channel->ready_to_destroy(); });
            const bool workersDone = worker_->target.server->drained();
            if (statesDone && workersDone) {
                wire_.releaseTransportRetirement();
            }
            if (statesDone && !wire_.stopStatus().stopping) {
                wire_.requestStop();
                anyProgress = true;
            }
            wire_.pollStop();
            if (statesDone && wire_.stopStatus().complete() && workersDone) {
                running_ = false;
            }
        } else if (exhaustedBudget) {
            // The QUIC wire owner bounds each turn. Queue a coalesced follow-up
            // so a long run of ready buffer work cannot strand the connection
            // after the last UDP/timer completion.
            wake();
        }
        transportActivityForPump_ = false;
        return anyProgress;
    } catch (...) {
        transportActivityForPump_ = false;
        report_failure(std::current_exception());
        return false;
    }
}

bool http3_worker_runtime::pump_worker(worker_link& worker) noexcept {
    bool progress{};
    try {
        for (auto& connection : worker.connections) {
            progress = connection.pump_admission(pendingOffers_) || progress;
            progress = connection.pump_local(transportActivityForPump_) || progress;
        }
        progress = pump_responses(worker) || progress;
        const bool response_drained = worker.response_buffer_drained &&
                                      !worker.pending_response && !worker.pending_response_control;
        for (auto& connection : worker.connections) {
            progress = connection.retire(response_drained) || progress;
        }
    } catch (...) {
        report_failure(std::current_exception());
    }
    return progress;
}

bool http3_worker_runtime::pump_responses(worker_link& worker) noexcept {
    bool progress = false;
    worker.response_buffer_drained = false;
    // Dispatch and this driver run on the same worker. Capacity notifications
    // defer continuations; the synchronous control-pop/data-acquire sequence
    // cannot run a producer between its two SPSC lane reads.
    std::size_t processed = 0;
    for (;;) {
        if (worker.pending_response_control) {
            auto* connection = find_connection(worker, worker.pending_response_control->id);
            if (connection && !connection->accept_response_control(*worker.pending_response_control)) {
                break;
            }
            worker.pending_response_control.reset();
            ++processed;
            progress = true;
        }
        if (processed >= kResponseBufferPumpBudget) {
            break;
        }
        if (worker.pending_response) {
            const auto* critical = worker.pending_response->critical();
            auto* connection = critical
                                   ? find_connection(worker, critical->epoch, critical->connection_generation)
                                   : find_connection(worker, worker.pending_response->id());
            if (connection) {
                if (!connection->accept_response_data(*worker.pending_response)) {
                    break;
                }
            } else {
                worker.pending_response->release();
            }
            worker.pending_response.reset();
            ++processed;
            progress = true;
        }
        if (processed >= kResponseBufferPumpBudget) {
            // Leave remaining buffer entries for the next protocol turn; the
            // outer worker pump schedules a bounded continuation while work remains.
            break;
        }
        if (!worker.pending_response_control) {
            http3_stream_control control;
            if (worker.target.server->response_buffer().try_receive_control(control)) {
                worker.pending_response_control = control;
                progress = true;
                continue;
            }
        }
        if (!worker.pending_response) {
            http3_stream_buffer::borrowed_block block;
            if (worker.target.server->response_buffer().try_receive(block)) {
                worker.pending_response.emplace(std::move(block));
                progress = true;
                continue;
            }
        }
        if (!worker.target.server->response_buffer().has_pending()) {
            worker.response_buffer_drained = true;
            break;
        }
    }
    return progress;
}
http3_connection_driver* http3_worker_runtime::find_connection(
    worker_link& worker, http3_stream_id id) noexcept {
    return find_connection(worker, id.epoch, id.connection_generation);
}

http3_connection_driver* http3_worker_runtime::find_connection(
    worker_link& worker, std::uint64_t epoch, std::uint64_t generation) noexcept {
    const auto found = std::ranges::find_if(worker.connections, [epoch, generation](const http3_connection_driver& value) {
        return value.matches({epoch, generation});
    });
    return found == worker.connections.end() ? nullptr : &*found;
}

}  // namespace ruvia::detail
