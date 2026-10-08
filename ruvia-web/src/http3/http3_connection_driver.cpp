#include "http3/http3_connection_driver.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/http/Http3LocalCriticalStreams.h"
#include "ruvia/http/Http3PeerStreams.h"

#include "http3/Http3QuicSocketAddress.h"
#include "server/HttpServerOptionsValidation.h"

namespace ruvia::detail {
namespace {
constexpr auto server_shutdown_code = Http3ConnectionErrorCode::kNoError;
constexpr auto protocol_failure_code = Http3ConnectionErrorCode::kInternalError;
constexpr std::size_t input_stream_pump_budget = 64;
bool phase_timeout_expired(std::optional<std::chrono::milliseconds> timeout,
    std::chrono::steady_clock::time_point last_activity,
    std::chrono::steady_clock::time_point now) noexcept {
    return timeout && now >= last_activity && now - last_activity >= *timeout;
}
}  // namespace

http3_connection_driver::http3_connection_driver(std::pmr::memory_resource* resource)
    : resource_(resource),
      state_(nullptr),
      request_buffer_(nullptr),
      wire_(nullptr),
      remote_address_(resource),
      streams_(resource),
      push_streams_(resource),
      output_(nullptr, PmrObjectDeleter<Http3ServerStreamOutput>{resource}),
      critical_(nullptr, PmrObjectDeleter<Http3CriticalStreamDriver>{resource}) {}

http3_connection_driver::http3_connection_driver(std::pmr::memory_resource* resource,
    http3_connection_state& state, http3_stream_buffer& request_buffer,
    Http3QuicWireOwner& wire, http3_connection_driver_config config)
    : http3_connection_driver(resource) {
    state_ = &state;
    request_buffer_ = &request_buffer;
    wire_ = &wire;
    config_ = config;
}
http3_connection_driver::http3_connection_driver(http3_connection_driver&& other) noexcept
    : http3_connection_driver(other.resource_) {
    if (other.executor_installed_ || other.transport_id_ || other.output_) {
        std::terminate();
    }
    state_ = other.state_;
    request_buffer_ = other.request_buffer_;
    wire_ = other.wire_;
    config_ = other.config_;
    identity_ = other.identity_;
    remote_address_ = std::move(other.remote_address_);
    streams_ = std::move(other.streams_);
    push_streams_ = std::move(other.push_streams_);
    next_input_stream_index_ = other.next_input_stream_index_;
    next_tunnel_handshake_stream_index_ = other.next_tunnel_handshake_stream_index_;
    tunnel_handshake_scan_remaining_ = other.tunnel_handshake_scan_remaining_;
    pending_tunnel_handshakes_ = other.pending_tunnel_handshakes_;
    tunnel_handshake_scan_dirty_ = other.tunnel_handshake_scan_dirty_;
    stopping_ = other.stopping_;
}

void http3_connection_driver::install_executor() noexcept {
    state_->set_transport_executor({this, [](void* context, http3_connection_identity identity,
                                              const Http3ServerConnection::TransportIntent& intent) noexcept {
                                        auto& self = *static_cast<http3_connection_driver*>(context);
                                        return self.matches(identity) ? self.execute_intent(intent)
                                                                      : http3_connection_state::intent_execution_result{.outcome = http3_connection_state::execution_outcome::stale};
                                    }});
    executor_installed_ = true;
}

bool http3_connection_driver::matches(http3_connection_identity identity) const noexcept {
    return has_generation() && identity_ == identity;
}

void http3_connection_driver::release_generation() noexcept {
    if (transport_id_ || output_) {
        std::terminate();
    }
    identity_ = {};
    handshake_deadline_.reset();
    remote_address_.clear();
    streams_.clear();
    push_streams_.clear();
    critical_.reset();
    admission_planner_.reset();
    drain_deadline_.reset();
    close_error_code_.reset();
    next_input_stream_index_ = 0;
    next_tunnel_handshake_stream_index_ = 0;
    tunnel_handshake_scan_remaining_ = 0;
    pending_tunnel_handshakes_ = 0;
    tunnel_handshake_scan_dirty_ = false;
    admitted_request_count_ = 0;
    peer_unidirectional_stream_count_ = 0;
    rejected_request_count_ = 0;
    goaway_queued_ = false;
    goaway_bytes_accepted_ = false;
    graceful_close_started_ = false;
    graceful_close_abandoned_ = false;
    close_started_ = false;
}

bool http3_connection_driver::pump_admission(std::pmr::vector<ruvia::quic_initial_offer>& offers) {
    if (state_->slot_reusable()) {
        if (!has_generation()) {
            return false;
        }
        release_generation();
        if (!stopping_ && state_->reset() != http3_connection_state::status::changed) {
            std::terminate();
        }
        return true;
    }
    bool progress{};
    if (!has_generation()) {
        // The handler can reserve a generation before transport startup throws.
        // Stop closes admission, not ownership of that existing reservation.
        const auto identity = stopping_ && state_->admission() == http3_connection_state::admission_phase::reserved
                                  ? state_->identity()
                                  : state_->available_identity();
        if (identity) {
            identity_ = *identity;
            progress = true;
        }
    }
    if (has_generation() && !bound()) {
        progress = (stopping_ ? retire_unbound() : admit(offers)) || progress;
    }
    if (bound() && !protocol_ready() && !close_started_) {
        if (state_->admission() == http3_connection_state::admission_phase::handler_attached) {
            progress = prepare_protocol() || progress;
        } else if (state_->admission() == http3_connection_state::admission_phase::rejected) {
            close_connection(protocol_failure_code);
            progress = true;
        }
    }
    return progress;
}

void http3_connection_driver::request_stop() noexcept {
    stopping_ = true;
    if (has_generation()) {
        if (bound()) {
            close_connection(server_shutdown_code);
        } else {
            (void)retire_unbound();
        }
    }
}

void http3_connection_driver::transport_failure() noexcept {
    if (bound()) {
        close_connection(protocol_failure_code);
    } else {
        (void)retire_unbound();
    }
}

void http3_connection_driver::observe_transport(std::chrono::steady_clock::time_point now) noexcept {
    if (!transport_id_) {
        return;
    }
    try {
        const auto info = wire_->transport()->server().connection(*transport_id_).info();
        if ((handshake_deadline_ && now >= *handshake_deadline_ && !info.quic_handshake_complete) ||
            info.state == ruvia::quic_connection_state::failed || info.state == ruvia::quic_connection_state::retired) {
            transport_failure();
        }
    } catch (const ruvia::quic_error&) {
        transport_failure();
    }
}

ruvia::quic_packet_result http3_connection_driver::write_packet(std::span<std::byte> bytes,
    std::chrono::steady_clock::time_point now) {
    // The bounded acceptance scan must finish before any queued HEAD can become
    // peer-visible, even when the response was accepted late in this turn.
    if (pending_tunnel_handshakes_ != 0 && tunnel_handshake_scan_remaining_ != 0 &&
        !close_started_ && !graceful_close_started_) {
        return {.status = ruvia::quic_operation_status::would_block};
    }
    return wire_->transport()->server().connection(*transport_id_).write_packet(bytes, now);
}

bool http3_connection_driver::pump_local(bool transport_activity) {
    if (!protocol_ready() || close_started_ || graceful_close_started_) {
        return false;
    }
    if (!wire_->transport()) {
        close_connection(server_shutdown_code);
        return true;
    }
    try {
        const auto state = wire_->transport()->server().connection(*transport_id_).info().state;
        if (state == ruvia::quic_connection_state::failed || state == ruvia::quic_connection_state::retired ||
            state == ruvia::quic_connection_state::closing || state == ruvia::quic_connection_state::draining) {
            close_connection(server_shutdown_code);
            return true;
        }
        bool progress = pump_input();
        if (close_started_) {
            return progress;
        }
        progress = pump_datagrams() || progress;
        return pump_output(transport_activity) || progress;
    } catch (const ruvia::quic_error&) {
        transport_failure();
        return true;
    } catch (...) {
        close_connection(protocol_failure_code);
        return true;
    }
}

bool http3_connection_driver::accept_response_control(const http3_stream_control& control) noexcept {
    if (!output_) {
        return true;
    }
    if (control.kind == http3_stream_control::kind::tunnel_established) {
        const auto info = output_->streamInfo(control.id.stream_id);
        if (accept_tunnel_established(control, info ? info->acceptedWireBytes : 0) ==
            tunnel_established_result::protocol_failure) {
            close_connection(protocol_failure_code);
        }
        return true;
    }
    const auto result = output_->acceptControl(control);
    if (result.status == Http3ServerStreamOutput::Status::kBackpressured) {
        return false;
    }
    if (result.status != Http3ServerStreamOutput::Status::kAccepted &&
        result.status != Http3ServerStreamOutput::Status::kFinDeferred &&
        result.status != Http3ServerStreamOutput::Status::kFinished &&
        result.status != Http3ServerStreamOutput::Status::kDuplicateFin &&
        result.status != Http3ServerStreamOutput::Status::kClosedStream) {
        close_connection(protocol_failure_code);
    }
    return true;
}

bool http3_connection_driver::accept_response_data(http3_stream_buffer::borrowed_block& block) noexcept {
    if (!output_) {
        block.release();
        return true;
    }
    const auto* critical = block.critical();
    const auto critical_stream = critical && critical_ ? critical_->streamId(critical->kind) : std::nullopt;
    if (critical && !critical_stream) {
        return false;
    }
    const auto result = critical ? output_->acceptCriticalData(block, *critical_stream) : output_->acceptData(block);
    if (result.status == Http3ServerStreamOutput::Status::kBackpressured) {
        return false;
    }
    if (result.status != Http3ServerStreamOutput::Status::kAccepted && result.status != Http3ServerStreamOutput::Status::kClosedStream) {
        block.release();
        close_connection(protocol_failure_code);
    }
    return true;
}

bool http3_connection_driver::admit(std::pmr::vector<ruvia::quic_initial_offer>& offers) {
    auto* transport = wire_->transport();
    if (transport == nullptr || !has_generation() || bound() ||
        state_->transport_retired() || stopping_) {
        return false;
    }

    bool progress = false;
    try {
        if (!transport_id_.has_value()) {
            if (offers.empty()) {
                return false;
            }
            streams_.reserve(ruvia::quic_limits{}.max_streams);
            push_streams_.reserve(kHttp3ServerPushAllowance);
            const auto admitted = transport->admit_initial(
                offers.front(), std::chrono::steady_clock::now());
            if (admitted.status == ruvia::quic_operation_status::would_block ||
                admitted.status == ruvia::quic_operation_status::need_input) {
                return false;
            }
            offers.erase(offers.begin());
            if (admitted.status != ruvia::quic_operation_status::accepted) {
                return true;
            }
            transport_id_ = admitted.connection;

            handshake_deadline_ =
                std::chrono::steady_clock::now() + config_.handshake_timeout;
            progress = true;
        }

        auto& quic = wire_->transport()->server().connection(*transport_id_);
        const auto info = quic.info();
        if (info.state == ruvia::quic_connection_state::failed ||
            info.state == ruvia::quic_connection_state::retired) {
            transport->retire(*transport_id_);
            transport_id_.reset();
            handshake_deadline_.reset();
            return revoke_reservation() || progress;
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
            transport->retire(*transport_id_);
            transport_id_.reset();
            handshake_deadline_.reset();
            (void)revoke_reservation();
            return true;
        }
        remote_address_ = peer->address().to_string();
        const auto committed = state_->bind(identity_,
            {.remote_address = remote_address_,
                .client_certificate_subject = {},
                .remote_port = peer->port()},
            config_.local_settings, quic.max_datagram_payload_size());
        if (committed != http3_connection_state::status::changed) {
            throw std::runtime_error("HTTP/3 accepted connection could not publish its binding");
        }
        handshake_deadline_.reset();

        return true;
    } catch (const ruvia::quic_error&) {
        if (transport_id_.has_value()) {
            transport->retire(*transport_id_);
            transport_id_.reset();
        }
        handshake_deadline_.reset();
        return revoke_reservation() || progress;
    }
}

bool http3_connection_driver::retire_unbound() noexcept {
    if (!has_generation() || bound()) {
        return false;
    }
    bool progress = false;
    if (transport_id_.has_value()) {
        auto* transport = wire_->transport();
        if (transport == nullptr) {
            return false;
        }
        transport->retire(*transport_id_);
        transport_id_.reset();
        handshake_deadline_.reset();
        progress = true;
    }
    return revoke_reservation() || progress;
}

bool http3_connection_driver::revoke_reservation() noexcept {
    if (!has_generation() || bound() || state_->transport_retired()) {
        return false;
    }
    auto& state = *state_;
    if (state.revoke(identity_) != http3_connection_state::status::changed ||
        state.mark_transport_retired(identity_) != http3_connection_state::status::changed) {
        std::terminate();
    }

    return true;
}

bool http3_connection_driver::prepare_protocol() noexcept {
    if (state_->admission() != http3_connection_state::admission_phase::handler_attached ||
        !bound() || protocol_ready() || close_started_) {
        return false;
    }
    try {
        const auto prefixes = Http3LocalCriticalStreams::create(config_.local_settings);
        auto* transport = wire_->transport();
        auto planner = Http3ServerRequestAdmissionPlanner::create({.max_requests_per_connection = static_cast<std::uint64_t>(config_.max_requests_per_connection)});
        if (!prefixes || !planner || !transport || !transport_id_) {
            close_connection(protocol_failure_code);
            return true;
        }
        auto critical = makePmrObject<Http3CriticalStreamDriver>(resource_, *prefixes);
        auto& quic = wire_->transport()->server().connection(*transport_id_);
        auto output = makePmrObject<Http3ServerStreamOutput>(resource_, quic, resource_,
            identity_.epoch, identity_.connection_generation,
            Http3ServerStreamOutputConfig{
                .maxTrackedStreams = config_.max_requests_per_connection,
                .maxQueuedBlocks = config_.buffer_capacity,
                .maxDriveWorkItems = 16,
                .write_timeout = config_.write_timeout});
        admission_planner_.emplace(std::move(*planner));
        critical_ = std::move(critical);
        output_ = std::move(output);
    } catch (...) {
        close_connection(protocol_failure_code);
    }
    return true;
}

bool http3_connection_driver::pump_input() {
    auto& transport = *wire_->transport();
    auto& quic = transport.server().connection(*transport_id_);
    bool progress = false;

    const auto critical = critical_->drive(
        [&quic](Http3CriticalStreamDriver::Kind) {
            return quic.open_stream(true);
        },
        [&quic](std::uint64_t id, std::span<const char> bytes) {
            return quic.write_stream(id, std::as_bytes(bytes));
        });
    if (critical == Http3CriticalStreamDriver::Result::kFatal) {
        close_connection(protocol_failure_code);
        return true;
    }
    progress = critical == Http3CriticalStreamDriver::Result::kProgress;
    if (goaway_queued_ && critical == Http3CriticalStreamDriver::Result::kReady) {
        goaway_bytes_accepted_ = true;
    }

    const auto accepted = quic.accept_streams();
    if (accepted.status != ruvia::quic_operation_status::accepted &&
        accepted.status != ruvia::quic_operation_status::need_input &&
        accepted.status != ruvia::quic_operation_status::would_block) {
        close_connection(protocol_failure_code);
        return true;
    }
    for (std::size_t i = 0; i < accepted.size; ++i) {
        const auto stream_id = accepted.streams[i].stream_id;
        if (!accepted.streams[i].readable ||
            streams_.size() >= streams_.capacity()) {
            close_connection(Http3ConnectionErrorCode::kExcessiveLoad);
            return true;
        }
        if (isHttp3RequestStreamId(stream_id)) {
            if (!admission_planner_) {
                close_connection(protocol_failure_code);
                return true;
            }
            const auto decision = admission_planner_->admit(stream_id);
            if (decision.action != Http3ServerRequestAdmissionAction::kAdmit) {
                if (!goaway_queued_ && !announce_goaway()) {
                    return true;
                }
                if (!reject_request_stream(stream_id)) {
                    return true;
                }
                progress = true;
                continue;
            }
            ++admitted_request_count_;
        } else if (isHttp3ClientUnidirectionalStreamId(stream_id)) {
            if (peer_unidirectional_stream_count_ >=
                kHttp3PeerUnidirectionalStreamAllowance) {
                close_connection(Http3ConnectionErrorCode::kExcessiveLoad);
                return true;
            }
            ++peer_unidirectional_stream_count_;
        } else {
            close_connection(Http3ConnectionErrorCode::kStreamCreationError);
            return true;
        }

        streams_.emplace_back(resource_);
        auto& stream = streams_.back();
        stream.id = stream_id;
        stream.request_stream = isHttp3RequestStreamId(stream_id);
        stream.last_input_activity = std::chrono::steady_clock::now();
        if (stream.request_stream) {
            // Duplicate only the frame-boundary state needed to observe when
            // request-header timeout should transition to body timeout.
            stream.frame_tracker = makePmrObject<Http3StreamFrames>(resource_,
                Http3StreamKind::kRequest, resource_);
            if (admitted_request_count_ == config_.max_requests_per_connection) {
                if (!announce_goaway() || !seal_admission()) {
                    return true;
                }
            }
        }
        progress = true;
    }

    const auto stream_count = streams_.size();
    const auto start_input_index = stream_count == 0
                                       ? std::size_t{0}
                                       : next_input_stream_index_ % stream_count;
    const auto stream_turn_budget = (std::min)(stream_count, input_stream_pump_budget);
    std::size_t processed_streams = 0;
    for (; processed_streams < stream_turn_budget; ++processed_streams) {
        const auto stream_index = (start_input_index + processed_streams) % stream_count;
        auto& stream = streams_[stream_index];
        // Terminal input can still need one final worker notification (notably
        // write-timeout cancellation after FIN was already delivered).
        if (stream.pending_control) {
            if (stream.request_stream) {
                const auto sent = request_buffer_->try_send_control(*stream.pending_control);
                if (sent == http3_stream_buffer::control_result::full) {
                    continue;
                }
                if (sent == http3_stream_buffer::control_result::stopped) {
                    close_connection(server_shutdown_code);
                    return true;
                }
            } else {
                const auto result = state_->accept_peer_stream_control(*stream.pending_control);
                if (!result) {
                    close_connection(protocol_failure_code);
                    return true;
                }
                if (result->connectionCloseRequired) {
                    // Keep the real parser's error code in the worker close intent.
                    return true;
                }
            }
            stream.pending_control.reset();
            if (!stream.request_stream) {
                auto* stream_transport = wire_->transport();
                const auto closed = stream_transport == nullptr
                                        ? ruvia::quic_operation_status::retired
                                        : stream_transport->server().connection(*transport_id_).close_stream(stream.id);
                if (closed != ruvia::quic_operation_status::accepted &&
                    closed != ruvia::quic_operation_status::completed &&
                    closed != ruvia::quic_operation_status::retired) {
                    close_connection(protocol_failure_code);
                    return true;
                }
            }
            complete_input_terminal(stream.id);
            progress = true;
            continue;
        }
        if (stream.input_terminal) {
            continue;
        }

        if (stream.request_stream) {
            const auto timeout = stream.input_phase == stream_state::receive_phase::headers
                                     ? config_.request_header_timeout
                                 : stream.body_timeout_applies()
                                     ? config_.request_body_timeout
                                     : std::nullopt;
            if (phase_timeout_expired(timeout, stream.last_input_activity,
                    std::chrono::steady_clock::now())) {
                note_input_reset(stream.id);
                terminate_request_stream(stream.id,
                    static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestCancelled));
                stream.frame_tracker.reset();
                stream.pending_control = http3_stream_control{
                    .kind = http3_stream_control::kind::stream_reset,
                    .id = {identity_.epoch,
                        identity_.connection_generation, stream.id},
                    .value = stream.received_bytes,
                    .stream_reset_error_code = Http3ConnectionErrorCode::kRequestCancelled,
                };
                progress = true;
                continue;
            }
        }

        http3_stream_buffer::data_reservation reservation;
        stream.received_early_data = stream.received_early_data ||
                                     quic.stream_info(stream.id).received_early_data;
        const http3_stream_id message_id{identity_.epoch,
            identity_.connection_generation, stream.id, {}, stream.received_early_data};
        // Peer unidirectional input cannot wait for request DATA credits: those
        // credits can all be held by QPACK-blocked requests. The same HTTP parser
        // consumes this synchronous borrow and owns incomplete instructions.
        std::array<std::byte, http3_stream_buffer::max_block_bytes> peer_storage;
        auto writable_bytes = std::span<std::byte>(peer_storage);
        if (stream.request_stream) {
            const auto reserved = request_buffer_->reserve_data(message_id, reservation);
            if (reserved == http3_stream_buffer::reservation_result::full ||
                reserved == http3_stream_buffer::reservation_result::no_block) {
                continue;
            }
            if (reserved != http3_stream_buffer::reservation_result::reserved) {
                close_connection(server_shutdown_code);
                return true;
            }
            writable_bytes = reservation.writable_bytes();
        }
        auto writable = std::span<char>(reinterpret_cast<char*>(writable_bytes.data()),
            writable_bytes.size());
        const auto read = quic.read_stream(stream.id, std::as_writable_bytes(writable));
        switch (read.status) {
            case ruvia::quic_stream_read_status::data: {
                if (read.size == 0 || read.size > writable.size() ||
                    read.size > std::numeric_limits<std::uint64_t>::max() -
                                    stream.received_bytes) {
                    reservation.abort();
                    close_connection(protocol_failure_code);
                    return true;
                }
                if (stream.frame_tracker) {
                    // This duplicate framer only observes the first request HEADERS
                    // boundary to select header/body timeout phase. The worker's
                    // protocol owner is authoritative for framing and errors.
                    try {
                        const auto frame_status = stream.frame_tracker->feed(
                            std::span<const char>(writable.data(), read.size), false,
                            +[](void* context, Http3StreamFrameEvent event) {
                                auto& tracked = *static_cast<stream_state*>(context);
                                if (event.kind == Http3StreamFrameEventKind::kHeaders &&
                                    !event.trailers && event.endFrame) {
                                    tracked.input_phase = stream_state::receive_phase::body;
                                }
                            },
                            &stream);
                        if (frame_status != Http3StreamFrameStatus::kNeedMoreData ||
                            stream.input_phase == stream_state::receive_phase::body) {
                            stream.frame_tracker.reset();
                        }
                    } catch (...) {
                        // Losing timeout-phase observation must not duplicate
                        // protocol validation or prevent forwarding bytes to the worker.
                        stream.frame_tracker.reset();
                    }
                }
                if (stream.request_stream) {
                    if (reservation.commit(read.size) != http3_stream_buffer::commit_result::sent) {
                        close_connection(protocol_failure_code);
                        return true;
                    }
                } else {
                    const auto result = state_->accept_peer_stream_data(message_id, writable_bytes.first(read.size));
                    if (!result) {
                        close_connection(protocol_failure_code);
                        return true;
                    }
                    if (result->connectionCloseRequired) {
                        return true;
                    }
                }
                stream.received_bytes += read.size;
                stream.last_input_activity = std::chrono::steady_clock::now();
                progress = true;
                break;
            }
            case ruvia::quic_stream_read_status::would_block:
                reservation.abort();
                break;
            case ruvia::quic_stream_read_status::fin:
                reservation.abort();
                if (stream.frame_tracker) {
                    // FIN observation is only needed to retire this duplicate
                    // timeout-phase parser; the worker processes the real FIN.
                    try {
                        static_cast<void>(stream.frame_tracker->feed({}, true, +[](void* context, Http3StreamFrameEvent event) {
                                auto& tracked = *static_cast<stream_state*>(context);
                                if (event.kind == Http3StreamFrameEventKind::kHeaders &&
                                    !event.trailers && event.endFrame) {
                                    tracked.input_phase = stream_state::receive_phase::body;
                                } }, &stream));
                    } catch (...) {
                        // Observation failure does not change protocol handling.
                    }
                    stream.frame_tracker.reset();
                }
                note_peer_fin(stream.id);
                stream.pending_control = http3_stream_control{
                    .kind = http3_stream_control::kind::stream_fin,
                    .id = message_id,
                    .value = stream.received_bytes,
                };
                progress = true;
                break;
            case ruvia::quic_stream_read_status::reset:
                reservation.abort();
                note_input_reset(stream.id);
                stream.frame_tracker.reset();
                if (stream.request_stream) {
                    terminate_request_stream(stream.id,
                        static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestCancelled));
                }
                stream.pending_control = http3_stream_control{
                    .kind = http3_stream_control::kind::stream_reset,
                    .id = message_id,
                    .value = stream.received_bytes,
                    .stream_reset_error_code = static_cast<Http3ConnectionErrorCode>(
                        read.peer_reset_error_code.value_or(static_cast<std::uint64_t>(
                            Http3ConnectionErrorCode::kRequestCancelled))),
                };
                progress = true;
                break;
            default:
                reservation.abort();
                close_connection(protocol_failure_code);
                return true;
        }
    }
    const auto stream_count_before_retirement = streams_.size();
    std::erase_if(streams_, [](const stream_state& stream) {
        // Keep request-stream identity through the connection lifetime: its
        // deferred TunnelEstablished marker may be published after peer FIN.
        return stream.input_terminal && !stream.request_stream;
    });
    if (streams_.size() != stream_count_before_retirement) {
        next_input_stream_index_ = 0;
        next_tunnel_handshake_stream_index_ = 0;
        tunnel_handshake_scan_remaining_ = pending_tunnel_handshakes_ == 0
                                               ? 0
                                               : streams_.size();
        tunnel_handshake_scan_dirty_ = false;
    } else if (stream_count != 0) {
        next_input_stream_index_ =
            (start_input_index + processed_streams) % stream_count;
    }
    return progress || (processed_streams == stream_turn_budget &&
                           stream_turn_budget < stream_count);
}

void http3_connection_driver::terminate_request_stream(std::uint64_t stream_id, std::uint64_t error_code) {
    bool terminate_directly = output_ == nullptr;
    if (output_) {
        const auto before_cancel = output_->streamInfo(stream_id);
        if (before_cancel && before_cancel->sendFinAccepted) {
            // cancelStream closes only the receive side when the real local FIN
            // has already been accepted by QUIC. Never fall back to RESET_STREAM
            // based solely on the HTTP response's terminal state.
            const auto result = output_->cancelStream(stream_id, error_code);
            switch (result.status) {
                case Http3ServerStreamOutput::Status::kCancelled:
                case Http3ServerStreamOutput::Status::kClosedStream:
                case Http3ServerStreamOutput::Status::kConnectionClosed:
                case Http3ServerStreamOutput::Status::kStopped:
                    return;
                default:
                    close_connection(protocol_failure_code);
                    return;
            }
        }

        const auto result = output_->cancelStream(stream_id, error_code);
        switch (result.status) {
            case Http3ServerStreamOutput::Status::kCancelled:
            case Http3ServerStreamOutput::Status::kConnectionClosed:
            case Http3ServerStreamOutput::Status::kStopped:
                return;
            case Http3ServerStreamOutput::Status::kClosedStream: {
                const auto info = output_->streamInfo(stream_id);
                if (!info || info->state == Http3ServerStreamOutput::StreamState::kFinished) {
                    terminate_directly = true;
                } else {
                    return;
                }
                break;
            }
            default:
                close_connection(protocol_failure_code);
                return;
        }
    }

    if (terminate_directly && bound()) {
        try {
            const auto status = wire_->transport()->server().connection(*transport_id_).terminate_bidirectional_stream(stream_id, error_code);
            if (status != ruvia::quic_operation_status::accepted &&
                status != ruvia::quic_operation_status::completed &&
                status != ruvia::quic_operation_status::retired) {
                close_connection(protocol_failure_code);
            }
        } catch (...) {
            close_connection(protocol_failure_code);
        }
    }
}

http3_connection_driver::tunnel_established_result http3_connection_driver::accept_tunnel_established(
    const http3_stream_control& control, std::uint64_t accepted_wire_bytes) noexcept {
    const auto found = std::ranges::find_if(streams_,
        [&control](const stream_state& stream) { return stream.id == control.id.stream_id; });
    if (found == streams_.end()) {
        return tunnel_established_result::protocol_failure;
    }
    const auto result = found->accept_tunnel_established(control, identity_, accepted_wire_bytes);
    if (result != tunnel_established_result::accepted) {
        return result;
    }
    if (found->tunnel_established_barrier) {
        if (pending_tunnel_handshakes_ == std::numeric_limits<std::size_t>::max()) {
            std::terminate();
        }
        ++pending_tunnel_handshakes_;
        if (tunnel_handshake_scan_remaining_ == 0) {
            tunnel_handshake_scan_remaining_ = streams_.size();
            next_tunnel_handshake_stream_index_ = 0;
            tunnel_handshake_scan_dirty_ = false;
        } else if (tunnel_handshake_scan_remaining_ < streams_.size()) {
            tunnel_handshake_scan_dirty_ = true;
        }
    }
    return tunnel_established_result::accepted;
}

bool http3_connection_driver::confirm_tunnel_established(std::uint64_t stream_id,
    std::uint64_t accepted_wire_bytes) noexcept {
    const auto found = std::ranges::find_if(streams_,
        [stream_id](const stream_state& stream) { return stream.id == stream_id; });
    const bool confirmed = found != streams_.end() &&
                           found->confirm_tunnel_established(accepted_wire_bytes);
    if (!confirmed) {
        return false;
    }
    if (pending_tunnel_handshakes_ == 0) {
        std::terminate();
    }
    --pending_tunnel_handshakes_;
    if (pending_tunnel_handshakes_ == 0) {
        tunnel_handshake_scan_remaining_ = 0;
        tunnel_handshake_scan_dirty_ = false;
    }
    return true;
}

void http3_connection_driver::note_peer_fin(std::uint64_t stream_id) noexcept {
    const auto found = std::ranges::find_if(streams_,
        [stream_id](const stream_state& stream) { return stream.id == stream_id; });
    if (found != streams_.end()) {
        found->input_fin = true;
    }
}

void http3_connection_driver::note_input_reset(std::uint64_t stream_id) noexcept {
    const auto found = std::ranges::find_if(streams_,
        [stream_id](const stream_state& stream) { return stream.id == stream_id; });
    if (found == streams_.end()) {
        return;
    }
    found->input_reset = true;
    found->frame_tracker.reset();
    if (found->tunnel_established_barrier) {
        if (pending_tunnel_handshakes_ == 0) {
            std::terminate();
        }
        --pending_tunnel_handshakes_;
        found->tunnel_established_barrier.reset();
    }
    if (pending_tunnel_handshakes_ == 0) {
        tunnel_handshake_scan_remaining_ = 0;
        tunnel_handshake_scan_dirty_ = false;
    }
}

void http3_connection_driver::complete_input_terminal(std::uint64_t stream_id) noexcept {
    const auto found = std::ranges::find_if(streams_,
        [stream_id](const stream_state& stream) { return stream.id == stream_id; });
    if (found == streams_.end()) {
        return;
    }
    if (found->input_reset && found->tunnel_established_barrier) {
        if (pending_tunnel_handshakes_ == 0) {
            std::terminate();
        }
        --pending_tunnel_handshakes_;
        found->tunnel_established_barrier.reset();
    }
    found->input_terminal = true;
}

void http3_connection_driver::stop_request_input(std::uint64_t stream_id) noexcept {
    const auto found = std::ranges::find_if(streams_,
        [stream_id](const stream_state& stream) { return stream.id == stream_id; });
    if (found == streams_.end()) {
        return;
    }
    found->pending_control.reset();
    note_input_reset(stream_id);
    complete_input_terminal(stream_id);
}

bool http3_connection_driver::announce_goaway() noexcept {
    if (goaway_queued_) {
        return true;
    }
    if (!admission_planner_ || !critical_) {
        close_connection(protocol_failure_code);
        return false;
    }
    (void)admission_planner_->announceGoaway();
    if (!critical_->queueGoaway(admission_planner_->goawayId())) {
        close_connection(protocol_failure_code);
        return false;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        config_.drain_timeout);
    drain_deadline_ = timeout > std::chrono::steady_clock::time_point::max() - now
                          ? std::chrono::steady_clock::time_point::max()
                          : now + timeout;
    goaway_queued_ = true;

    return true;
}

bool http3_connection_driver::seal_admission() noexcept {
    if (state_->admission_seal().has_value()) {
        return true;
    }
    if (!goaway_queued_ || !admission_planner_ ||
        admitted_request_count_ != config_.max_requests_per_connection) {
        close_connection(protocol_failure_code);
        return false;
    }
    const auto sealed = state_->seal_admission(identity_,
        config_.max_requests_per_connection, admission_planner_->goawayId());
    if (sealed != http3_connection_state::status::changed) {
        close_connection(protocol_failure_code);
        return false;
    }

    return true;
}

bool http3_connection_driver::reject_request_stream(std::uint64_t stream_id) {
    if (rejected_request_count_ >= kHttp3PostGoawayRequestAllowance) {
        close_connection(Http3ConnectionErrorCode::kExcessiveLoad);
        return false;
    }
    auto* transport = wire_->transport();
    if (transport == nullptr) {
        close_connection(protocol_failure_code);
        return false;
    }
    const auto status = wire_->transport()->server().connection(*transport_id_).terminate_bidirectional_stream(stream_id, static_cast<std::uint64_t>(Http3ConnectionErrorCode::kRequestRejected));
    if (status != ruvia::quic_operation_status::accepted &&
        status != ruvia::quic_operation_status::completed &&
        status != ruvia::quic_operation_status::retired) {
        close_connection(protocol_failure_code);
        return false;
    }
    ++rejected_request_count_;
    return true;
}

Http3DatagramReceiveStatus http3_connection_driver::plan_datagram_receive(
    const Http3DatagramView& datagram) const noexcept {
    const auto stream = std::ranges::find(streams_, datagram.streamId, &stream_state::id);
    return planHttp3DatagramReceive(datagram,
        {.localH3Datagram = config_.local_settings.h3Datagram,
            .streamExists = stream != streams_.end(),
            .receiveOpen = stream != streams_.end() && !stream->input_terminal && !stream->input_reset,
            .supportsDatagrams = stream != streams_.end() && stream->tunnel_established});
}

bool http3_connection_driver::pump_datagrams() {
    auto& quic = wire_->transport()->server().connection(*transport_id_);
    auto& channel = *state_;
    bool progress{};
    std::array<std::byte, http3_connection_state::max_datagram_bytes> input{};
    for (std::size_t count = 0; count < http3_connection_state::datagram_capacity; ++count) {
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
            close_connection(static_cast<Http3ConnectionErrorCode>(kHttp3DatagramErrorCode));
            break;
        }
        const auto planned = plan_datagram_receive(*decoded);
        if (planned == Http3DatagramReceiveStatus::kConnectionError) {
            close_connection(static_cast<Http3ConnectionErrorCode>(kHttp3DatagramErrorCode));
            break;
        }
        if (planned == Http3DatagramReceiveStatus::kDeliver) {
            (void)channel.publish_request_datagram(identity_, decoded->streamId, bytes);
        }
    }
    for (std::size_t count = 0; count < http3_connection_state::datagram_capacity; ++count) {
        http3_connection_state::datagram output;
        if (channel.pop_response_datagram(output) != http3_connection_state::status::changed) {
            break;
        }
        progress = true;
        if (output.identity.epoch != identity_.epoch ||
            output.identity.connection_generation != identity_.connection_generation) {
            continue;
        }
        (void)quic.write_datagram(output.bytes());
    }
    return progress;
}

bool http3_connection_driver::pump_output(bool transport_activity) {
    if (output_ == nullptr) {
        return false;
    }
    if (transport_activity) {
        output_->notifyTransportActivity();
    }
    const auto result = output_->drive();
    if (result.status == Http3ServerStreamOutput::Status::kTransportError ||
        result.status == Http3ServerStreamOutput::Status::kFinalSizeError ||
        result.status == Http3ServerStreamOutput::Status::kCapacityExhausted ||
        result.status == Http3ServerStreamOutput::Status::kUnsafeToRelease) {
        close_connection(protocol_failure_code);
    }
    for (auto& stream : streams_) {
        if (!stream.request_stream || stream.input_reset || stream.write_timeout_notified) {
            continue;
        }
        const auto info = output_->streamInfo(stream.id);
        if (info && info->timedOut) {
            // Reset takes precedence over a queued FIN; if FIN was already
            // delivered, input_terminal does not suppress this cancellation.
            stream.write_timeout_notified = true;
            note_input_reset(stream.id);
            stream.frame_tracker.reset();
            stream.pending_control = http3_stream_control{
                .kind = http3_stream_control::kind::stream_reset,
                .id = {identity_.epoch,
                    identity_.connection_generation, stream.id},
                .value = stream.received_bytes,
                .stream_reset_error_code = Http3ConnectionErrorCode::kRequestCancelled,
            };
        }
    }
    if (result.acceptedBytes != 0 && pending_tunnel_handshakes_ != 0) {
        if (tunnel_handshake_scan_remaining_ == 0) {
            tunnel_handshake_scan_remaining_ = streams_.size();
            next_tunnel_handshake_stream_index_ = 0;
            tunnel_handshake_scan_dirty_ = false;
        } else if (tunnel_handshake_scan_remaining_ < streams_.size()) {
            tunnel_handshake_scan_dirty_ = true;
        }
    }

    bool handshake_progress = false;
    const auto stream_count = streams_.size();
    if (pending_tunnel_handshakes_ != 0 &&
        tunnel_handshake_scan_remaining_ != 0) {
        if (stream_count == 0) {
            std::terminate();
        }
        const auto scan_budget = (std::min)({stream_count,
            tunnel_handshake_scan_remaining_, input_stream_pump_budget});
        for (std::size_t scanned = 0; scanned < scan_budget; ++scanned) {
            const auto stream_index = next_tunnel_handshake_stream_index_ % stream_count;
            next_tunnel_handshake_stream_index_ = (stream_index + 1) % stream_count;
            --tunnel_handshake_scan_remaining_;
            auto& stream = streams_[stream_index];
            if (!stream.tunnel_established_barrier) {
                continue;
            }
            const auto info = output_->streamInfo(stream.id);
            if (info && confirm_tunnel_established(stream.id,
                            info->acceptedWireBytes)) {
                handshake_progress = true;
            }
        }
        if (pending_tunnel_handshakes_ == 0) {
            tunnel_handshake_scan_remaining_ = 0;
            tunnel_handshake_scan_dirty_ = false;
        } else if (tunnel_handshake_scan_remaining_ == 0 &&
                   tunnel_handshake_scan_dirty_) {
            tunnel_handshake_scan_remaining_ = stream_count;
            next_tunnel_handshake_stream_index_ = 0;
            tunnel_handshake_scan_dirty_ = false;
        }
    }
    return result.madeProgress || result.needsReschedule || handshake_progress ||
           tunnel_handshake_scan_remaining_ != 0;
}

bool http3_connection_driver::retire(bool response_drained) {
    if (!transport_id_.has_value() || !bound() ||
        state_->transport_retired()) {
        return false;
    }

    if (goaway_queued_ && !close_started_ &&
        !graceful_close_started_) {
        const auto now = std::chrono::steady_clock::now();
        if (drain_deadline_ && now >= *drain_deadline_) {
            // The deadline also bounds an unsealed admission window: without
            // all N eligible low-ID requests, graceful retirement is forbidden.
            close_connection(server_shutdown_code);
        } else if (state_->admission_seal().has_value() && goaway_bytes_accepted_ &&
                   state_->worker_drained() && response_drained &&
                   output_ != nullptr && output_->liveStreamCount() == 0) {
            graceful_close_started_ = true;
        }
    }
    if (graceful_close_started_ && drain_deadline_ &&
        std::chrono::steady_clock::now() >= *drain_deadline_) {
        graceful_close_abandoned_ = true;
    }
    if (!close_started_ && !graceful_close_started_) {
        return false;
    }

    auto* transport = wire_->transport();
    if (transport == nullptr) {
        return false;
    }
    auto& quic = wire_->transport()->server().connection(*transport_id_);
    bool force_local_retirement = stopping_ || graceful_close_abandoned_ ||
                                  (close_started_ && drain_deadline_ &&
                                      std::chrono::steady_clock::now() >= *drain_deadline_);
    const auto state = quic.info().state;
    if (state == ruvia::quic_connection_state::failed) {
        force_local_retirement = true;
    }
    if (state != ruvia::quic_connection_state::retired &&
        state != ruvia::quic_connection_state::closing &&
        state != ruvia::quic_connection_state::draining) {
        static constexpr std::string_view graceful_reason = "HTTP/3 drain complete";
        static constexpr std::string_view close_reason = "HTTP/3 connection closed";
        const auto reason = graceful_close_started_ ? graceful_reason : close_reason;
        try {
            const auto close = quic.close({
                .kind = ruvia::quic_close_kind::application,
                .code = static_cast<std::uint64_t>(
                    close_error_code_.value_or(server_shutdown_code)),
                .reason = {reason.data(), reason.size()},
            });
            if (close == ruvia::quic_operation_status::would_block ||
                close == ruvia::quic_operation_status::need_input) {
                if (!force_local_retirement) {
                    return false;
                }
            }
            if (close != ruvia::quic_operation_status::accepted &&
                close != ruvia::quic_operation_status::completed &&
                close != ruvia::quic_operation_status::closing &&
                close != ruvia::quic_operation_status::draining &&
                close != ruvia::quic_operation_status::retired) {
                graceful_close_abandoned_ = true;
                force_local_retirement = true;
            }
        } catch (const ruvia::quic_error&) {
            graceful_close_abandoned_ = true;
            force_local_retirement = true;
        }
    }
    if (!force_local_retirement &&
        quic.info().state != ruvia::quic_connection_state::retired) {
        return false;
    }

    // Stop the stream writer before retiring its connection so all borrowed
    // buffer blocks are returned only after HTTP accepts or invalidates writes.
    if (output_ != nullptr) {
        const auto stopped = output_->stop();
        if (stopped.status == Http3ServerStreamOutput::Status::kUnsafeToRelease) {
            return false;
        }
        output_.reset();
    }
    critical_.reset();
    transport->retire(*transport_id_);
    transport_id_.reset();
    handshake_deadline_.reset();
    if (state_->mark_transport_retired(identity_) !=
        http3_connection_state::status::changed) {
        std::terminate();
    }

    return true;
}
void http3_connection_driver::close_connection(Http3ConnectionErrorCode reason) noexcept {
    if (!transport_id_ || close_started_) {
        return;
    }
    if (graceful_close_started_) {
        graceful_close_abandoned_ = true;
        return;
    }
    close_started_ = true;
    close_error_code_ = reason;
    if (!drain_deadline_) {
        drain_deadline_ = std::chrono::steady_clock::now() + config_.drain_timeout;
    }
    if (auto* transport = wire_->transport(); transport != nullptr) {
        static constexpr std::string_view close_reason = "HTTP/3 connection closing";
        try {
            (void)transport->server().connection(*transport_id_).close({
                .kind = ruvia::quic_close_kind::application,
                .code = static_cast<std::uint64_t>(reason),
                .reason = {close_reason.data(), close_reason.size()},
            });
        } catch (...) {
            graceful_close_abandoned_ = true;
        }
    }
}

http3_connection_state::intent_execution_result http3_connection_driver::execute_intent(const Http3ServerConnection::TransportIntent& intent) noexcept {
    http3_connection_state::intent_execution_result result{
        .outcome = state_->transport_retired()
                       ? http3_connection_state::execution_outcome::transport_retired
                       : http3_connection_state::execution_outcome::executed};
    if (intent.token.kind == Http3ServerConnection::TransportIntentKind::kOpenPushStream) {
        result.push_stream.emplace();
        if (state_->transport_retired() || stopping_) {
            result.push_stream->status = Http3ServerConnection::PushStreamOpenResult::Status::kStopped;
        } else if (intent.token.id.push_id && output_ &&
                   push_streams_.size() < push_streams_.capacity()) {
            try {
                auto* transport = wire_->transport();
                if (!transport) {
                    result.push_stream->status = Http3ServerConnection::PushStreamOpenResult::Status::kStopped;
                } else {
                    auto& quic = wire_->transport()->server().connection(*transport_id_);
                    const auto opened = quic.open_stream(true);
                    if (opened.status == ruvia::quic_operation_status::accepted) {
                        const auto registered = output_->registerPushStream(opened.stream_id, *intent.token.id.push_id);
                        if (registered.status == Http3ServerStreamOutput::Status::kAccepted) {
                            *result.push_stream = {.status = Http3ServerConnection::PushStreamOpenResult::Status::kOpened,
                                .streamId = opened.stream_id};
                            push_streams_.push_back({opened.stream_id, *intent.token.id.push_id});
                        } else {
                            (void)quic.close_stream(opened.stream_id);
                        }
                    } else if (opened.status != ruvia::quic_operation_status::would_block &&
                               opened.status != ruvia::quic_operation_status::need_input) {
                        close_connection(protocol_failure_code);
                        result.push_stream->status = Http3ServerConnection::PushStreamOpenResult::Status::kStopped;
                    }
                }
            } catch (...) {
                close_connection(protocol_failure_code);
                result.push_stream->status = Http3ServerConnection::PushStreamOpenResult::Status::kStopped;
            }
        }
    }
    if (!state_->transport_retired()) {
        if (intent.token.kind == Http3ServerConnection::TransportIntentKind::kConnectionClose) {
            close_connection(intent.connectionErrorCode.value_or(protocol_failure_code));
        } else if (intent.token.kind == Http3ServerConnection::TransportIntentKind::kStreamReset) {
            try {
                terminate_request_stream(intent.token.id.stream_id,
                    static_cast<std::uint64_t>(intent.streamResetErrorCode));
            } catch (...) {
                close_connection(protocol_failure_code);
            }
            if (!intent.token.id.push_id) {
                stop_request_input(intent.token.id.stream_id);
            }
        }
    }
    return result;
}

}  // namespace ruvia::detail
