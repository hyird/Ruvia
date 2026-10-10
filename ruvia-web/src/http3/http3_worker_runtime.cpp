#include "http3/http3_worker_runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

#include <asio/bind_allocator.hpp>
#include <asio/post.hpp>

#include "ruvia/core/async.h"
#include "ruvia/http/http3_local_critical_streams.h"
#include "ruvia/http/http3_peer_streams.h"
#include "ruvia/http/http3_stream_frames.h"

#include "http3/http3_quic_socket_address.h"
#include "http3/http3_worker_server.h"
#include "server/http_server_listener.h"
#include "server/http_server_options_validation.h"

namespace ruvia::detail {
namespace {

constexpr auto monitor_interval = std::chrono::milliseconds(10);
constexpr std::size_t response_buffer_pump_budget = 64;

std::size_t connection_capacity(const http3_worker_runtime::worker_target& worker_value) {
    if (worker_value.server_ == nullptr || worker_value.max_connections_ == 0 ||
        worker_value.buffer_capacity_ == 0 || worker_value.max_requests_per_connection_ == 0) {
        throw std::invalid_argument("invalid HTTP/3 worker target");
    }
    return worker_value.max_connections_;
}

ruvia::quic_server_config transport_config(
    const http3_worker_runtime::worker_target& worker_value, ruvia::quic_cid_partition partition) {
    ruvia::quic_server_config config;
    config.max_active_connections_ = connection_capacity(worker_value);
    config.max_pending_connections_ = config.max_active_connections_;
    config.cid_partition_ = partition;
    config.limits_.max_lifetime_peer_streams_ =
        http3_transport_lifetime_stream_capacity(worker_value.max_requests_per_connection_);
    config.local_transport_parameters_.idle_timeout_ms_ =
        worker_value.idle_timeout_ ? static_cast<std::uint64_t>(worker_value.idle_timeout_->count()) : 0;
    config.local_transport_parameters_.max_datagram_frame_size_ = 65536;
    // New peer paths remain on this worker through its CID partition.
    config.local_transport_parameters_.disable_active_migration_ = false;
    return config;
}

}  // namespace

bool queue_http3_initial_offer(
    std::pmr::vector<ruvia::quic_initial_offer>& pending_offers,
    const ruvia::quic_initial_offer& offer) noexcept {
    if (std::ranges::any_of(pending_offers, [&offer](const ruvia::quic_initial_offer& pending) {
            return pending.offer_id_ == offer.offer_id_;
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
        http3_worker_datagram_endpoint& endpoint_;
        ~reservation_guard() {
            endpoint_.cancel_packet();
        }
    } guard_value{endpoint};
    const auto packet = server.write_version_negotiation(plan, packet_buffer);
    if (packet.status_ == ruvia::quic_operation_status::would_block) {
        return http3_worker_datagram_endpoint::pump_result::idle;
    }
    if (packet.status_ != ruvia::quic_operation_status::accepted || packet.size_ == 0 ||
        packet.size_ > packet_buffer.size()) {
        throw std::runtime_error("QUIC server failed to serialize Version Negotiation");
    }
    const auto source_value = to_udp_endpoint(from_quic_address(packet.local_));
    const auto peer = to_udp_endpoint(from_quic_address(packet.peer_));
    if ((source_value.index() != 0) || (peer.index() != 0)) {
        throw std::runtime_error("QUIC Version Negotiation returned invalid UDP addresses");
    }
    const auto sent = endpoint.send_datagram(
        std::span<const std::byte>(packet_buffer).first(packet.size_), std::get<0>(source_value), std::get<0>(peer));
    if (sent == http3_worker_datagram_endpoint::pump_result::error) {
        throw std::system_error(endpoint.error(), "send HTTP/3 Version Negotiation packet");
    }
    return sent;
}

http3_worker_runtime::worker_link::worker_link(std::pmr::memory_resource* resource,
    http3_worker_runtime& runtime, worker_target configured)
    : target_(std::move(configured)),
      request_buffer_(target_.buffer_capacity_, target_.buffer_capacity_, target_.buffer_capacity_,
          resource, {.ready_ = {target_.server_, [](void* context_value, std::uint8_t) noexcept {
                                    static_cast<http3_worker_server*>(context_value)->wake();
                                }},
                        .capacity_ = {&runtime, [](void* context_value, std::uint8_t) noexcept {
                                          static_cast<http3_worker_runtime*>(context_value)->wake();
                                      }}}),
      states_(resource),
      state_views_(resource),
      connections_(resource) {
    states_.reserve(target_.max_connections_);
    state_views_.reserve(target_.max_connections_);
    connections_.reserve(target_.max_connections_);
    for (std::size_t i = 0; i < target_.max_connections_; ++i) {
        states_.push_back(make_pmr_object<http3_connection_state>(resource,
            http3_connection_state::local_change_callback{&runtime, [](void* context_value) noexcept {
                                                              auto& owner_value = *static_cast<http3_worker_runtime*>(context_value);
                                                              owner_value.wake();
                                                              owner_value.worker_->target_.server_->wake();
                                                          }},
            resource));
        state_views_.push_back(states_.back().get());
        connections_.emplace_back(resource, *states_.back(), request_buffer_, runtime.wire_,
            http3_connection_driver_config{
                .max_requests_per_connection_ = target_.max_requests_per_connection_,
                .buffer_capacity_ = target_.buffer_capacity_,
                .request_header_timeout_ = target_.request_header_timeout_,
                .request_body_timeout_ = target_.request_body_timeout_,
                .write_timeout_ = target_.write_timeout_,
                .drain_timeout_ = runtime.drain_timeout_,
                .handshake_timeout_ = runtime.handshake_timeout_,
                .local_settings_ = runtime.local_settings_});
        connections_.back().install_executor();
    }
}

http3_worker_runtime::http3_worker_runtime(ruvia::worker_runtime_context& runtime,
    asio::ip::udp::endpoint local, const http_server_listener_definition::tls_type& tls_config_value,
    const http3_listen_config& http3_config, worker_target worker_value,
    http3_datagram_channel& datagrams, ruvia::quic_cid_partition partition,
    failure_notification failure)
    : io_context_(runtime.io_context()),
      owner_thread_(std::this_thread::get_id()),
      memory_(),
      protocol_signal_(runtime.handle()),
      notification_tasks_(runtime.handle(), {.resource_ = memory_.resource()}),
      bind_address_(local.address()),
      tls_(tls_config_value, memory_.resource()),
      wire_(io_context_, datagrams, local, tls_, transport_config(worker_value, partition),
          memory_.resource(), {this, [](void* context_value, http3_quic_server_transport& transport, http3_worker_datagram_endpoint& endpoint) noexcept {
                                   auto& self = *static_cast<http3_worker_runtime*>(context_value);
                                   const bool progress_value = self.pump_protocol(&transport, &endpoint);
                                   if (self.failure_) {
                                       return http3_quic_wire_owner::protocol_pump_result_type::fatal;
                                   }
                                   return progress_value
                                              ? http3_quic_wire_owner::protocol_pump_result_type::progress
                                              : http3_quic_wire_owner::protocol_pump_result_type::idle;
                               }}),
      worker_(nullptr, pmr_object_deleter<worker_link>{memory_.resource()}),
      datagrams_(&datagrams),
      pending_offers_(memory_.resource()),
      monitor_timer_(io_context_),
      failure_notification_(failure),
      drain_timeout_(http3_config.drain_timeout_),
      handshake_timeout_(http3_config.handshake_timeout_),
      local_settings_{.qpack_max_table_capacity_ = http3_config.qpack_.max_table_capacity_, .qpack_blocked_streams_ = http3_config.qpack_.max_blocked_streams_, .enable_connect_protocol_ = true, .h3_datagram_ = true} {
    try {
        if (drain_timeout_ <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("HTTP/3 drain timeout must be greater than zero");
        }
        pending_offers_.reserve(connection_capacity(worker_value));
        worker_ = make_pmr_object<worker_link>(memory_.resource(), memory_.resource(), *this, worker_value);
        wire_.prepare();
        wire_.defer_transport_retirement();
        // The caller owns constructor rollback until the final nonthrowing step.
        datagrams.worker_start();
    } catch (...) {
        // Detach before the caller publishes its single cold rollback ACK.
        wire_.request_stop();
        wire_.poll_stop();
        if (!wire_.stop_status().complete()) {
            std::terminate();
        }
        datagrams_ = nullptr;
        throw;
    }
}

http3_worker_runtime::~http3_worker_runtime() {
    require_owner_thread();
    if (running_ || monitor_scheduled_ || !protocol_drained() || datagrams_ != nullptr) {
        std::terminate();
    }
}

void http3_worker_runtime::stage() {
    require_owner_thread();
    if (staged_) {
        throw std::logic_error("HTTP/3 worker runtime already staged");
    }
    auto& worker_value = *worker_;
    if (!worker_value.target_.server_->stage_install({.request_buffer_ = &worker_value.request_buffer_,
            .connections_ = worker_value.state_views_,
            .protocol_ready_ = {this, [](void* context_value, std::uint8_t) noexcept {
                                    static_cast<http3_worker_runtime*>(context_value)->wake();
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
    wire_.request_drive();
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
    const bool already_stopping = stopping_;
    request_stop_on_owner();
    if (!already_stopping) {
        (void)pump_protocol();
    }
    wire_.poll_stop();
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
    const bool states_done = std::ranges::all_of(worker_->states_,
        [](const auto& channel) { return channel->ready_to_destroy(); });
    return states_done && worker_->target_.server_->drained() &&
           wire_.stop_status().complete() && !monitor_scheduled_;
}

bool http3_worker_runtime::drained() const noexcept {
    return protocol_drained() && datagrams_ == nullptr &&
           (!datagram_run_started_ || datagram_run_retired_);
}

asio::ip::udp::endpoint http3_worker_runtime::local_endpoint() const {
    require_owner_thread();
    return {bind_address_, wire_.bound_port()};
}

task<void> http3_worker_runtime::join() {
    require_owner_thread();
    while (!drained()) {
        co_await protocol_signal_.wait();
    }
}

task<void> http3_worker_runtime::run_notifications() {
    // This is the worker's only native datagram wait. Core registers the wait
    // and rechecks its pending latch; local protocol work uses worker_signal.
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
            if (status == worker_notification_wait_status::closed) {
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

task<void> http3_worker_runtime::run_datagrams() {
    require_owner_thread();
    if (datagrams_ == nullptr || datagram_run_started_) {
        std::terminate();
    }
    datagram_run_started_ = true;
    std::size_t inline_turns{};
    const auto post_continuation = [this](auto completion) {
        asio::post(io_context_.get_executor(), asio::bind_allocator(
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
                wire_.poll_stop();
            } else {
                wire_.request_drive();
            }
            if (!protocol_drained()) {
                if (++inline_turns == 16) {
                    inline_turns = 0;
                    const auto yielded = co_await ruvia::async_asio(post_continuation);
                    if (yielded.error_code()) {
                        throw std::system_error(yielded.error_code(), "yield HTTP/3 protocol worker");
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
        wire_.poll_stop();
        if (!protocol_drained()) {
            if (++inline_turns == 16) {
                inline_turns = 0;
                const auto yielded = co_await ruvia::async_asio(post_continuation);
                if (yielded.error_code()) {
                    report_failure(std::make_exception_ptr(std::system_error(
                        yielded.error_code(), "yield retiring HTTP/3 protocol worker")));
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
    if (!worker_->request_buffer_.quiescent() ||
        !worker_->target_.server_->response_buffer().quiescent()) {
        std::terminate();
    }
    worker_->request_buffer_.set_local_notifications({});
    worker_->target_.server_->response_buffer().set_local_notifications({});
    // wire stop has detached its endpoint; no borrowed channel survives ACK.
    auto* channel = std::exchange(datagrams_, nullptr);
    // The final wire stop may complete in a poll_stop() after the last pump, so
    // the drained protocol retires here, where every stop path converges.
    running_ = false;
    channel->worker_close();
}

void http3_worker_runtime::require_owner_thread() const noexcept {
    if (std::this_thread::get_id() != owner_thread_) {
        std::terminate();
    }
}

void http3_worker_runtime::request_stop_on_owner() noexcept {
    require_owner_thread();
    if (stopping_) {
        return;
    }
    stopping_ = true;
    pending_offers_.clear();
    for (auto& state : worker_->states_) {
        state->stop_admission();
    }
    worker_->target_.server_->request_stop();
    for (auto& connection : worker_->connections_) {
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
    if (failure_reported_) {
        return;
    }
    failure_reported_ = true;
    if (failure_notification_.notify_ != nullptr) {
        failure_notification_.notify_(failure_notification_.context_, failure_);
    }
    schedule_monitor();
}

void http3_worker_runtime::schedule_monitor() noexcept {
    require_owner_thread();
    if (monitor_scheduled_ || (!running_ && !stopping_) || protocol_drained()) {
        return;
    }
    monitor_scheduled_ = true;
    try {
        monitor_timer_.expires_after(monitor_interval);
        monitor_timer_.async_wait([this](const asio::error_code& error) noexcept {
            monitor(error);
        });
    } catch (...) {
        monitor_scheduled_ = false;
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
    monitor_scheduled_ = false;
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
            wire_.poll_stop();
        } else if (running_) {
            wire_.request_drive();
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
        auto& worker_value = worker_;
        bool any_progress = false;
        bool exhausted_budget = true;
        transport_activity_for_pump_ = wire_.consume_transport_activity();
        if (transport == nullptr) {
            transport = wire_.transport();
        }
        if (!stopping_ && transport != nullptr && endpoint != nullptr) {
            const auto now = std::chrono::steady_clock::now();
            (void)transport->server().handle_expiry(now);
            if (const auto received = endpoint->receive_slot()) {
                const auto local = to_http3_quic_datagram_address(received->local_destination_);
                const auto peer = to_http3_quic_datagram_address(received->peer_);
                if ((local.index() == 0) && (peer.index() == 0)) {
                    ruvia::quic_server_route route;
                    try {
                        route = transport->route_datagram(received->bytes_, std::get<0>(local), std::get<0>(peer));
                    } catch (const ruvia::quic_error& error) {
                        if (error.code() != ruvia::quic_error_code::protocol_failure) {
                            throw;
                        }
                    }
                    if (route.kind_ == ruvia::quic_server_route_kind::initial_offer) {
                        (void)queue_http3_initial_offer(pending_offers_, route.offer_);
                    } else if (route.kind_ == ruvia::quic_server_route_kind::version_negotiation) {
                        if (send_http3_version_negotiation(transport->server(),
                                route.version_negotiation_, *endpoint) ==
                            http3_worker_datagram_endpoint::pump_result::pending) {
                            any_progress = true;
                        }
                    } else if (route.kind_ == ruvia::quic_server_route_kind::existing_connection) {
                        const ruvia::quic_datagram_view datagram{
                            received->bytes_, to_quic_address(std::get<0>(local)), to_quic_address(std::get<0>(peer))};
                        try {
                            (void)transport->server().receive(route.connection_, datagram, now);
                        } catch (const ruvia::quic_error&) {
                            for (auto& connection : worker_value->connections_) {
                                if (connection.transport_token() == route.connection_) {
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
                any_progress = true;
                if (endpoint->receive_slot()) {
                    // Preserve a local continuation when the wire turn exhausts
                    // its budget; another UDP/native edge is not required.
                    wake();
                }
            }
            for (auto& connection : worker_value->connections_) {
                connection.observe_transport(now);
            }
        }
        for (std::size_t pass = 0; pass < pump_budget_; ++pass) {
            const bool progress_value = pump_worker(*worker_);
            // One observed edge wakes every connection in the first pass only.
            // Reusing it during local retries restarts blocked output scans
            // without new ACK/credit, delaying the packet flush below.
            transport_activity_for_pump_ = false;
            if (!progress_value) {
                exhausted_budget = false;
                break;
            }
            any_progress = true;
        }

        if (!stopping_ && transport != nullptr && endpoint != nullptr) {
            const auto count = worker_->connections_.size();
            const auto budget = (std::min)(count, std::size_t{16});
            for (std::size_t attempt_value = 0; attempt_value < budget; ++attempt_value) {
                const auto index = next_packet_connection_++ % count;
                auto& connection = worker_->connections_[index];
                if (!connection.transport_token()) {
                    continue;
                }
                const auto packet_buffer = endpoint->packet_buffer();
                if (packet_buffer.empty()) {
                    break;  // Pause production only. RX/expiry/local pump ran above.
                }
                struct reservation_guard final {
                    http3_worker_datagram_endpoint& endpoint_;
                    ~reservation_guard() {
                        endpoint_.cancel_packet();
                    }
                } guard_value{*endpoint};
                try {
                    const auto packet = connection.write_packet(packet_buffer,
                        std::chrono::steady_clock::now());
                    if (packet.size_ == 0) {
                        continue;
                    }
                    const auto source_value = to_udp_endpoint(from_quic_address(packet.local_));
                    const auto peer = to_udp_endpoint(from_quic_address(packet.peer_));
                    if ((source_value.index() != 0) || (peer.index() != 0) || packet.size_ > packet_buffer.size()) {
                        connection.transport_failure();
                        continue;
                    }
                    const auto sent = endpoint->send_datagram(
                        std::span<const std::byte>(packet_buffer).first(packet.size_), std::get<0>(source_value), std::get<0>(peer));
                    if (sent == http3_worker_datagram_endpoint::pump_result::error) {
                        throw std::system_error(endpoint->error(), "send HTTP/3 QUIC packet");
                    }
                    any_progress = true;
                    // Further production takes another cooperative local turn.
                    wake();
                } catch (const ruvia::quic_error&) {
                    connection.transport_failure();
                }
            }
        }

        if (stopping_) {
            const bool states_done = std::ranges::all_of(worker_->states_,
                [](const auto& channel) { return channel->ready_to_destroy(); });
            const bool workers_done = worker_->target_.server_->drained();
            if (states_done && workers_done) {
                wire_.release_transport_retirement();
            }
            if (states_done && !wire_.stop_status().stopping_) {
                wire_.request_stop();
                any_progress = true;
            }
            wire_.poll_stop();
        } else if (exhausted_budget) {
            // The QUIC wire owner bounds each turn. Queue a coalesced follow-up
            // so a long run of ready buffer work cannot strand the connection
            // after the last UDP/timer completion.
            wake();
        }
        transport_activity_for_pump_ = false;
        return any_progress;
    } catch (...) {
        transport_activity_for_pump_ = false;
        report_failure(std::current_exception());
        return false;
    }
}

bool http3_worker_runtime::pump_worker(worker_link& worker_value) noexcept {
    bool progress_value{};
    try {
        for (auto& connection : worker_value.connections_) {
            progress_value = connection.pump_admission(pending_offers_) || progress_value;
            progress_value = connection.pump_local(transport_activity_for_pump_) || progress_value;
        }
        progress_value = pump_responses(worker_value) || progress_value;
        const bool response_drained = worker_value.response_buffer_drained_ &&
                                      !worker_value.pending_response_ && !worker_value.pending_response_control_;
        for (auto& connection : worker_value.connections_) {
            progress_value = connection.retire(response_drained) || progress_value;
        }
    } catch (...) {
        report_failure(std::current_exception());
    }
    return progress_value;
}

bool http3_worker_runtime::pump_responses(worker_link& worker_value) noexcept {
    bool progress_value = false;
    worker_value.response_buffer_drained_ = false;
    // Dispatch and this driver run on the same worker. Capacity notifications
    // defer continuations; the synchronous control-pop/data-acquire sequence
    // cannot run a producer between its two SPSC lane reads.
    std::size_t processed = 0;
    for (;;) {
        if (worker_value.pending_response_control_) {
            auto* connection = find_connection(worker_value, worker_value.pending_response_control_->id_);
            if (connection && !connection->accept_response_control(*worker_value.pending_response_control_)) {
                break;
            }
            worker_value.pending_response_control_.reset();
            ++processed;
            progress_value = true;
        }
        if (processed >= response_buffer_pump_budget) {
            break;
        }
        if (worker_value.pending_response_) {
            const auto* critical = worker_value.pending_response_->critical();
            auto* connection = critical
                                   ? find_connection(worker_value, critical->epoch_, critical->connection_generation_)
                                   : find_connection(worker_value, worker_value.pending_response_->id());
            if (connection) {
                if (!connection->accept_response_data(*worker_value.pending_response_)) {
                    break;
                }
            } else {
                worker_value.pending_response_->release();
            }
            worker_value.pending_response_.reset();
            ++processed;
            progress_value = true;
        }
        if (processed >= response_buffer_pump_budget) {
            // Leave remaining buffer entries for the next protocol turn; the
            // outer worker pump schedules a bounded continuation while work remains.
            break;
        }
        if (!worker_value.pending_response_control_) {
            http3_stream_control control;
            if (worker_value.target_.server_->response_buffer().try_receive_control(control)) {
                worker_value.pending_response_control_ = control;
                progress_value = true;
                continue;
            }
        }
        if (!worker_value.pending_response_) {
            http3_stream_buffer::borrowed_block block;
            if (worker_value.target_.server_->response_buffer().try_receive(block)) {
                worker_value.pending_response_.emplace(std::move(block));
                progress_value = true;
                continue;
            }
        }
        if (!worker_value.target_.server_->response_buffer().has_pending()) {
            worker_value.response_buffer_drained_ = true;
            break;
        }
    }
    return progress_value;
}
http3_connection_driver* http3_worker_runtime::find_connection(
    worker_link& worker_value, http3_stream_id id) noexcept {
    return find_connection(worker_value, id.epoch_, id.connection_generation_);
}

http3_connection_driver* http3_worker_runtime::find_connection(
    worker_link& worker_value, std::uint64_t epoch, std::uint64_t generation) noexcept {
    const auto found = std::ranges::find_if(worker_value.connections_, [epoch, generation](const http3_connection_driver& value) {
        return value.matches({epoch, generation});
    });
    return found == worker_value.connections_.end() ? nullptr : &*found;
}

}  // namespace ruvia::detail
