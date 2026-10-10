#include "http3/http3_connection_driver.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

#include "ruvia/http/http3_local_critical_streams.h"
#include "ruvia/http/http3_peer_streams.h"

#include "http3/http3_quic_socket_address.h"
#include "server/http_server_options_validation.h"

namespace ruvia::detail {
namespace {
constexpr auto server_shutdown_code = http3_connection_error_code::no_error;
constexpr auto protocol_failure_code = http3_connection_error_code::internal_error;
// RFC 9000 Section 20.1: the server refused to accept a new connection.
constexpr std::uint64_t quic_connection_refused = 0x02;
constexpr std::size_t input_stream_pump_budget = 64;
bool phase_timeout_expired(std::optional<std::chrono::milliseconds> timeout,
    std::chrono::steady_clock::time_point last_activity,
    std::chrono::steady_clock::time_point now) noexcept {
    return timeout && now >= last_activity && now - last_activity >= *timeout;
}
std::chrono::steady_clock::time_point deadline_after(std::chrono::steady_clock::time_point now,
    std::chrono::milliseconds timeout) noexcept {
    using clock_duration = std::chrono::steady_clock::duration;
    constexpr auto representable = std::chrono::duration_cast<std::chrono::milliseconds>(clock_duration::max());
    if (timeout >= representable) {
        return std::chrono::steady_clock::time_point::max();
    }
    const auto duration = std::chrono::duration_cast<clock_duration>(timeout);
    return duration > std::chrono::steady_clock::time_point::max() - now
               ? std::chrono::steady_clock::time_point::max()
               : now + duration;
}
// QUIC accepts DATAGRAM payloads up to its validated 65527-byte limit, while
// HTTP Datagrams here are bounded by the connection state. read_datagram keeps
// an oversized head queued; RFC 9297 permits dropping it, which must remove it.
bool discard_oversized_datagram(ruvia::quic_connection& quic) noexcept {
    std::array<std::byte, ruvia::quic_limits{}.max_datagram_size_> scratch;
    return quic.read_datagram(scratch).status_ == ruvia::quic_datagram_status::received;
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
      output_(nullptr, pmr_object_deleter<http3_server_stream_output>{resource}),
      critical_(nullptr, pmr_object_deleter<http3_critical_stream_driver>{resource}) {}

http3_connection_driver::http3_connection_driver(std::pmr::memory_resource* resource,
    http3_connection_state& state_value, http3_stream_buffer& request_buffer,
    http3_quic_wire_owner& wire, http3_connection_driver_config config)
    : http3_connection_driver(resource) {
    state_ = &state_value;
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
    state_->set_transport_executor({this, [](void* context_value, http3_connection_identity identity,
                                              const http3_server_connection::transport_intent_type& intent) noexcept {
                                        auto& self = *static_cast<http3_connection_driver*>(context_value);
                                        return self.matches(identity) ? self.execute_intent(intent)
                                                                      : http3_connection_state::intent_execution_result{.outcome_ = http3_connection_state::execution_outcome::stale};
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
    close_code_.reset();
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
    bool progress_value{};
    if (!has_generation()) {
        // The handler can reserve a generation before transport startup throws.
        // Stop closes admission, not ownership of that existing reservation.
        const auto identity = stopping_ && state_->admission() == http3_connection_state::admission_phase::reserved
                                  ? state_->identity()
                                  : state_->available_identity();
        if (identity) {
            identity_ = *identity;
            progress_value = true;
        }
    }
    if (has_generation() && !bound()) {
        progress_value = (stopping_ ? retire_unbound() : admit(offers)) || progress_value;
    }
    if (bound() && !protocol_ready() && !close_started_) {
        if (state_->admission() == http3_connection_state::admission_phase::handler_attached) {
            progress_value = prepare_protocol() || progress_value;
        } else if (state_->admission() == http3_connection_state::admission_phase::rejected) {
            // Capacity and shutdown refuse the connection before HTTP/3 starts;
            // only a local construction failure is an internal error.
            const auto rejection = state_->rejection();
            if (rejection == http3_connection_state::reject_reason::capacity ||
                rejection == http3_connection_state::reject_reason::stopping) {
                refuse_connection();
            } else {
                close_connection(protocol_failure_code);
            }
            progress_value = true;
        }
    }
    return progress_value;
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
        if ((handshake_deadline_ && now >= *handshake_deadline_ && !info.quic_handshake_complete_) ||
            info.state_ == ruvia::quic_connection_state::failed || info.state_ == ruvia::quic_connection_state::retired) {
            transport_failure();
        }
    } catch (const ruvia::quic_error&) {
        transport_failure();
    }
}

ruvia::quic_packet_result http3_connection_driver::write_packet(std::span<std::byte> bytes_value,
    std::chrono::steady_clock::time_point now) {
    // The bounded acceptance scan must finish before any queued HEAD can become
    // peer-visible, even when the response was accepted late in this turn.
    if (pending_tunnel_handshakes_ != 0 && tunnel_handshake_scan_remaining_ != 0 &&
        !close_started_ && !graceful_close_started_) {
        return {.status_ = ruvia::quic_operation_status::would_block};
    }
    return wire_->transport()->server().connection(*transport_id_).write_packet(bytes_value, now);
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
        const auto state_value = wire_->transport()->server().connection(*transport_id_).info().state_;
        if (state_value == ruvia::quic_connection_state::failed || state_value == ruvia::quic_connection_state::retired ||
            state_value == ruvia::quic_connection_state::closing || state_value == ruvia::quic_connection_state::draining) {
            close_connection(server_shutdown_code);
            return true;
        }
        bool progress_value = pump_input();
        if (close_started_) {
            return progress_value;
        }
        progress_value = pump_datagrams() || progress_value;
        return pump_output(transport_activity) || progress_value;
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
    if (control.kind_ == http3_stream_control::kind::tunnel_established) {
        const auto info = output_->stream_info(control.id_.stream_id_);
        if (accept_tunnel_established(control, info ? info->accepted_wire_bytes_ : 0) ==
            tunnel_established_result::protocol_failure) {
            close_connection(protocol_failure_code);
        }
        return true;
    }
    const auto result_value = output_->accept_control(control);
    if (result_value.status_ == http3_server_stream_output::status_type::backpressured) {
        return false;
    }
    if (result_value.status_ != http3_server_stream_output::status_type::accepted &&
        result_value.status_ != http3_server_stream_output::status_type::fin_deferred &&
        result_value.status_ != http3_server_stream_output::status_type::finished &&
        result_value.status_ != http3_server_stream_output::status_type::duplicate_fin &&
        result_value.status_ != http3_server_stream_output::status_type::closed_stream) {
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
    const auto critical_stream = critical && critical_ ? critical_->stream_id(critical->kind_) : std::nullopt;
    if (critical && !critical_stream) {
        return false;
    }
    const auto result_value = critical ? output_->accept_critical_data(block, *critical_stream) : output_->accept_data(block);
    if (result_value.status_ == http3_server_stream_output::status_type::backpressured) {
        return false;
    }
    if (result_value.status_ != http3_server_stream_output::status_type::accepted && result_value.status_ != http3_server_stream_output::status_type::closed_stream) {
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

    bool progress_value = false;
    try {
        if (!transport_id_.has_value()) {
            if (offers.empty()) {
                return false;
            }
            streams_.reserve(ruvia::quic_limits{}.max_streams_);
            push_streams_.reserve(http3_server_push_allowance);
            ruvia::quic_server_admit_result admitted;
            try {
                admitted = transport->admit_initial(offers.front(), std::chrono::steady_clock::now());
            } catch (...) {
                // A failed admission consumes its offer, and the transport has
                // already released the pending Initial. Retrying the same Initial
                // (for example a rejected ClientHello) fails again and would block
                // every later offer on this worker; would_block stays retryable.
                offers.erase(offers.begin());
                throw;
            }
            if (admitted.status_ == ruvia::quic_operation_status::would_block ||
                admitted.status_ == ruvia::quic_operation_status::need_input) {
                return false;
            }
            offers.erase(offers.begin());
            if (admitted.status_ != ruvia::quic_operation_status::accepted) {
                return true;
            }
            transport_id_ = admitted.connection_;

            handshake_deadline_ = deadline_after(std::chrono::steady_clock::now(), config_.handshake_timeout_);
            progress_value = true;
        }

        auto& quic = wire_->transport()->server().connection(*transport_id_);
        const auto info = quic.info();
        if (info.state_ == ruvia::quic_connection_state::failed ||
            info.state_ == ruvia::quic_connection_state::retired) {
            transport->retire(*transport_id_);
            transport_id_.reset();
            handshake_deadline_.reset();
            return revoke_reservation() || progress_value;
        }
        const auto tls_info = quic.tls_handshake().info();
        const bool negotiated_h3 = tls_info.negotiated_alpn_.size() == 2 &&
                                   tls_info.negotiated_alpn_[0] == std::byte{static_cast<unsigned char>('h')} &&
                                   tls_info.negotiated_alpn_[1] == std::byte{static_cast<unsigned char>('3')};
        if (!info.tls_handshake_complete_ || !info.quic_handshake_complete_ ||
            !info.confirmed_ || !negotiated_h3) {
            return progress_value;
        }
        const auto peer = to_udp_endpoint(from_quic_address(info.peer_address_));
        if ((peer.index() != 0)) {
            transport->retire(*transport_id_);
            transport_id_.reset();
            handshake_deadline_.reset();
            (void)revoke_reservation();
            return true;
        }
        remote_address_ = std::get<0>(peer).address().to_string();
        const auto committed = state_->bind(identity_,
            {.remote_address_ = remote_address_,
                .client_certificate_subject_ = {},
                .remote_port_ = std::get<0>(peer).port()},
            config_.local_settings_, quic.max_datagram_payload_size());
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
        return revoke_reservation() || progress_value;
    }
}

bool http3_connection_driver::retire_unbound() noexcept {
    if (!has_generation() || bound()) {
        return false;
    }
    bool progress_value = false;
    if (transport_id_.has_value()) {
        auto* transport = wire_->transport();
        if (transport == nullptr) {
            return false;
        }
        transport->retire(*transport_id_);
        transport_id_.reset();
        handshake_deadline_.reset();
        progress_value = true;
    }
    return revoke_reservation() || progress_value;
}

bool http3_connection_driver::revoke_reservation() noexcept {
    if (!has_generation() || bound() || state_->transport_retired()) {
        return false;
    }
    auto& state_value = *state_;
    if (state_value.revoke(identity_) != http3_connection_state::status::changed ||
        state_value.mark_transport_retired(identity_) != http3_connection_state::status::changed) {
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
        const auto prefixes = http3_local_critical_streams::create(config_.local_settings_);
        auto* transport = wire_->transport();
        auto planner = http3_server_request_admission_planner::create({.max_requests_per_connection_ = static_cast<std::uint64_t>(config_.max_requests_per_connection_)});
        if ((prefixes.index() != 0) || (planner.index() != 0) || !transport || !transport_id_) {
            close_connection(protocol_failure_code);
            return true;
        }
        auto critical = make_pmr_object<http3_critical_stream_driver>(resource_, std::get<0>(prefixes));
        auto& quic = wire_->transport()->server().connection(*transport_id_);
        auto output = make_pmr_object<http3_server_stream_output>(resource_, quic, resource_,
            identity_.epoch_, identity_.connection_generation_,
            http3_server_stream_output_config{
                .max_tracked_streams_ = config_.max_requests_per_connection_,
                .max_queued_blocks_ = config_.buffer_capacity_,
                .max_drive_work_items_ = 16,
                .write_timeout_ = config_.write_timeout_});
        admission_planner_.emplace(std::move(std::get<0>(planner)));
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
    bool progress_value = false;

    const auto critical = critical_->drive(
        [&quic](http3_critical_stream_driver::kind_type) {
            return quic.open_stream(true);
        },
        [&quic](std::uint64_t id, std::span<const char> bytes_value) {
            return quic.write_stream(id, std::as_bytes(bytes_value));
        });
    if (critical == http3_critical_stream_driver::result_type::fatal) {
        close_connection(protocol_failure_code);
        return true;
    }
    progress_value = critical == http3_critical_stream_driver::result_type::progress;
    if (goaway_queued_ && critical == http3_critical_stream_driver::result_type::ready) {
        goaway_bytes_accepted_ = true;
    }

    const auto accepted = quic.accept_streams();
    if (accepted.status_ != ruvia::quic_operation_status::accepted &&
        accepted.status_ != ruvia::quic_operation_status::need_input &&
        accepted.status_ != ruvia::quic_operation_status::would_block) {
        close_connection(protocol_failure_code);
        return true;
    }
    for (std::size_t i = 0; i < accepted.size_; ++i) {
        const auto stream_id = accepted.streams_[i].stream_id_;
        if (!accepted.streams_[i].readable_ ||
            streams_.size() >= streams_.capacity()) {
            close_connection(http3_connection_error_code::excessive_load);
            return true;
        }
        if (is_http3_request_stream_id(stream_id)) {
            if (!admission_planner_) {
                close_connection(protocol_failure_code);
                return true;
            }
            const auto decision = admission_planner_->admit(stream_id);
            if (decision.action_ != http3_server_request_admission_action::admit) {
                if (!goaway_queued_ && !announce_goaway()) {
                    return true;
                }
                if (!reject_request_stream(stream_id)) {
                    return true;
                }
                progress_value = true;
                continue;
            }
            ++admitted_request_count_;
        } else if (is_http3_client_unidirectional_stream_id(stream_id)) {
            if (peer_unidirectional_stream_count_ >=
                http3_peer_unidirectional_stream_allowance) {
                close_connection(http3_connection_error_code::excessive_load);
                return true;
            }
            ++peer_unidirectional_stream_count_;
        } else {
            close_connection(http3_connection_error_code::stream_creation_error);
            return true;
        }

        streams_.emplace_back(resource_);
        auto& stream = streams_.back();
        stream.id_ = stream_id;
        stream.request_stream_ = is_http3_request_stream_id(stream_id);
        stream.last_input_activity_ = std::chrono::steady_clock::now();
        if (stream.request_stream_) {
            // Duplicate only the frame-boundary state needed to observe when
            // request-header timeout should transition to body timeout.
            stream.frame_tracker_ = make_pmr_object<http3_stream_frames>(resource_,
                http3_stream_kind::request, resource_);
            if (admitted_request_count_ == config_.max_requests_per_connection_) {
                if (!announce_goaway() || !seal_admission()) {
                    return true;
                }
            }
        }
        progress_value = true;
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
        if (stream.pending_control_) {
            if (stream.request_stream_) {
                const auto sent = request_buffer_->try_send_control(*stream.pending_control_);
                if (sent == http3_stream_buffer::control_result::full) {
                    continue;
                }
                if (sent == http3_stream_buffer::control_result::stopped) {
                    close_connection(server_shutdown_code);
                    return true;
                }
            } else {
                const auto result_value = state_->accept_peer_stream_control(*stream.pending_control_);
                if (!result_value) {
                    close_connection(protocol_failure_code);
                    return true;
                }
                if (result_value->connection_close_required_) {
                    // Keep the real parser's error code in the worker close intent.
                    return true;
                }
            }
            stream.pending_control_.reset();
            if (!stream.request_stream_) {
                auto* stream_transport = wire_->transport();
                const auto closed = stream_transport == nullptr
                                        ? ruvia::quic_operation_status::retired
                                        : stream_transport->server().connection(*transport_id_).close_stream(stream.id_);
                if (closed != ruvia::quic_operation_status::accepted &&
                    closed != ruvia::quic_operation_status::completed &&
                    closed != ruvia::quic_operation_status::retired) {
                    close_connection(protocol_failure_code);
                    return true;
                }
            }
            complete_input_terminal(stream.id_);
            progress_value = true;
            continue;
        }
        if (stream.input_terminal_) {
            continue;
        }

        if (stream.request_stream_) {
            const auto timeout = stream.input_phase_ == stream_state::receive_phase::headers
                                     ? config_.request_header_timeout_
                                 : stream.body_timeout_applies()
                                     ? config_.request_body_timeout_
                                     : std::nullopt;
            if (phase_timeout_expired(timeout, stream.last_input_activity_,
                    std::chrono::steady_clock::now())) {
                note_input_reset(stream.id_);
                terminate_request_stream(stream.id_,
                    static_cast<std::uint64_t>(http3_connection_error_code::request_cancelled));
                stream.frame_tracker_.reset();
                stream.pending_control_ = http3_stream_control{
                    .kind_ = http3_stream_control::kind::stream_reset,
                    .id_ = {identity_.epoch_,
                        identity_.connection_generation_, stream.id_},
                    .value_ = stream.received_bytes_,
                    .stream_reset_error_code_ = http3_connection_error_code::request_cancelled,
                };
                progress_value = true;
                continue;
            }
        }

        http3_stream_buffer::data_reservation reservation;
        stream.received_early_data_ = stream.received_early_data_ ||
                                      quic.stream_info(stream.id_).received_early_data_;
        const http3_stream_id message_id{identity_.epoch_,
            identity_.connection_generation_, stream.id_, {}, stream.received_early_data_};
        // Peer unidirectional input cannot wait for request DATA credits: those
        // credits can all be held by QPACK-blocked requests. The same HTTP parser
        // consumes this synchronous borrow and owns incomplete instructions.
        std::array<std::byte, http3_stream_buffer::max_block_bytes> peer_storage;
        auto writable_bytes = std::span<std::byte>(peer_storage);
        if (stream.request_stream_) {
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
        const auto read = quic.read_stream(stream.id_, std::as_writable_bytes(writable));
        switch (read.status_) {
            case ruvia::quic_stream_read_status::data: {
                if (read.size_ == 0 || read.size_ > writable.size() ||
                    read.size_ > std::numeric_limits<std::uint64_t>::max() -
                                     stream.received_bytes_) {
                    reservation.abort();
                    close_connection(protocol_failure_code);
                    return true;
                }
                if (stream.frame_tracker_) {
                    // This duplicate framer only observes the first request HEADERS
                    // boundary to select header/body timeout phase. The worker's
                    // protocol owner is authoritative for framing and errors.
                    try {
                        const auto frame_status = stream.frame_tracker_->feed(
                            std::span<const char>(writable.data(), read.size_), false,
                            +[](void* context_value, http3_stream_frame_event event) {
                                auto& tracked = *static_cast<stream_state*>(context_value);
                                if (event.kind_ == http3_stream_frame_event_kind::headers &&
                                    !event.trailers_ && event.end_frame_) {
                                    tracked.input_phase_ = stream_state::receive_phase::body;
                                }
                            },
                            &stream);
                        if (frame_status != http3_stream_frame_status::need_more_data ||
                            stream.input_phase_ == stream_state::receive_phase::body) {
                            stream.frame_tracker_.reset();
                        }
                    } catch (...) {
                        // Losing timeout-phase observation must not duplicate
                        // protocol validation or prevent forwarding bytes to the worker.
                        stream.frame_tracker_.reset();
                    }
                }
                if (stream.request_stream_) {
                    if (reservation.commit(read.size_) != http3_stream_buffer::commit_result::sent) {
                        close_connection(protocol_failure_code);
                        return true;
                    }
                } else {
                    const auto result_value = state_->accept_peer_stream_data(message_id, writable_bytes.first(read.size_));
                    if (!result_value) {
                        close_connection(protocol_failure_code);
                        return true;
                    }
                    if (result_value->connection_close_required_) {
                        return true;
                    }
                }
                stream.received_bytes_ += read.size_;
                stream.last_input_activity_ = std::chrono::steady_clock::now();
                progress_value = true;
                break;
            }
            case ruvia::quic_stream_read_status::would_block:
                reservation.abort();
                break;
            case ruvia::quic_stream_read_status::fin:
                reservation.abort();
                if (stream.frame_tracker_) {
                    // FIN observation is only needed to retire this duplicate
                    // timeout-phase parser; the worker processes the real FIN.
                    try {
                        static_cast<void>(stream.frame_tracker_->feed({}, true, +[](void* context_value, http3_stream_frame_event event) {
                                auto& tracked = *static_cast<stream_state*>(context_value);
                                if (event.kind_ == http3_stream_frame_event_kind::headers &&
                                    !event.trailers_ && event.end_frame_) {
                                    tracked.input_phase_ = stream_state::receive_phase::body;
                                } }, &stream));
                    } catch (...) {
                        // Observation failure does not change protocol handling.
                    }
                    stream.frame_tracker_.reset();
                }
                note_peer_fin(stream.id_);
                stream.pending_control_ = http3_stream_control{
                    .kind_ = http3_stream_control::kind::stream_fin,
                    .id_ = message_id,
                    .value_ = stream.received_bytes_,
                };
                progress_value = true;
                break;
            case ruvia::quic_stream_read_status::reset:
                reservation.abort();
                note_input_reset(stream.id_);
                stream.frame_tracker_.reset();
                if (stream.request_stream_) {
                    terminate_request_stream(stream.id_,
                        static_cast<std::uint64_t>(http3_connection_error_code::request_cancelled));
                }
                stream.pending_control_ = http3_stream_control{
                    .kind_ = http3_stream_control::kind::stream_reset,
                    .id_ = message_id,
                    .value_ = stream.received_bytes_,
                    .stream_reset_error_code_ = static_cast<http3_connection_error_code>(
                        read.peer_reset_error_code_.value_or(static_cast<std::uint64_t>(
                            http3_connection_error_code::request_cancelled))),
                };
                progress_value = true;
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
        // deferred tunnel_established marker may be published after peer FIN.
        return stream.input_terminal_ && !stream.request_stream_;
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
    return progress_value || (processed_streams == stream_turn_budget &&
                                 stream_turn_budget < stream_count);
}

void http3_connection_driver::terminate_request_stream(std::uint64_t stream_id, std::uint64_t error_code) {
    bool terminate_directly = output_ == nullptr;
    if (output_) {
        const auto before_cancel = output_->stream_info(stream_id);
        if (before_cancel && before_cancel->send_fin_accepted_) {
            // cancel_stream closes only the receive side when the real local FIN
            // has already been accepted by QUIC. Never fall back to RESET_STREAM
            // based solely on the HTTP response's terminal state.
            const auto result_value = output_->cancel_stream(stream_id, error_code);
            switch (result_value.status_) {
                case http3_server_stream_output::status_type::cancelled:
                case http3_server_stream_output::status_type::closed_stream:
                case http3_server_stream_output::status_type::connection_closed:
                case http3_server_stream_output::status_type::stopped:
                    return;
                default:
                    close_connection(protocol_failure_code);
                    return;
            }
        }

        const auto result_value = output_->cancel_stream(stream_id, error_code);
        switch (result_value.status_) {
            case http3_server_stream_output::status_type::cancelled:
            case http3_server_stream_output::status_type::connection_closed:
            case http3_server_stream_output::status_type::stopped:
                return;
            case http3_server_stream_output::status_type::closed_stream: {
                const auto info = output_->stream_info(stream_id);
                if (!info || info->state_ == http3_server_stream_output::stream_state_type::finished) {
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
        [&control](const stream_state& stream) { return stream.id_ == control.id_.stream_id_; });
    if (found == streams_.end()) {
        return tunnel_established_result::protocol_failure;
    }
    const auto result_value = found->accept_tunnel_established(control, identity_, accepted_wire_bytes);
    if (result_value != tunnel_established_result::accepted) {
        return result_value;
    }
    if (found->tunnel_established_barrier_) {
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
        [stream_id](const stream_state& stream) { return stream.id_ == stream_id; });
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
        [stream_id](const stream_state& stream) { return stream.id_ == stream_id; });
    if (found != streams_.end()) {
        found->input_fin_ = true;
    }
}

void http3_connection_driver::note_input_reset(std::uint64_t stream_id) noexcept {
    const auto found = std::ranges::find_if(streams_,
        [stream_id](const stream_state& stream) { return stream.id_ == stream_id; });
    if (found == streams_.end()) {
        return;
    }
    found->input_reset_ = true;
    found->frame_tracker_.reset();
    if (found->tunnel_established_barrier_) {
        if (pending_tunnel_handshakes_ == 0) {
            std::terminate();
        }
        --pending_tunnel_handshakes_;
        found->tunnel_established_barrier_.reset();
    }
    if (pending_tunnel_handshakes_ == 0) {
        tunnel_handshake_scan_remaining_ = 0;
        tunnel_handshake_scan_dirty_ = false;
    }
}

void http3_connection_driver::complete_input_terminal(std::uint64_t stream_id) noexcept {
    const auto found = std::ranges::find_if(streams_,
        [stream_id](const stream_state& stream) { return stream.id_ == stream_id; });
    if (found == streams_.end()) {
        return;
    }
    if (found->input_reset_ && found->tunnel_established_barrier_) {
        if (pending_tunnel_handshakes_ == 0) {
            std::terminate();
        }
        --pending_tunnel_handshakes_;
        found->tunnel_established_barrier_.reset();
    }
    found->input_terminal_ = true;
}

void http3_connection_driver::stop_request_input(std::uint64_t stream_id) noexcept {
    const auto found = std::ranges::find_if(streams_,
        [stream_id](const stream_state& stream) { return stream.id_ == stream_id; });
    if (found == streams_.end()) {
        return;
    }
    found->pending_control_.reset();
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
    (void)admission_planner_->announce_goaway();
    if (!critical_->queue_goaway(admission_planner_->goaway_id())) {
        close_connection(protocol_failure_code);
        return false;
    }
    drain_deadline_ = deadline_after(std::chrono::steady_clock::now(), config_.drain_timeout_);
    goaway_queued_ = true;

    return true;
}

bool http3_connection_driver::seal_admission() noexcept {
    if (state_->admission_seal().has_value()) {
        return true;
    }
    if (!goaway_queued_ || !admission_planner_ ||
        admitted_request_count_ != config_.max_requests_per_connection_) {
        close_connection(protocol_failure_code);
        return false;
    }
    const auto sealed = state_->seal_admission(identity_,
        config_.max_requests_per_connection_, admission_planner_->goaway_id());
    if (sealed != http3_connection_state::status::changed) {
        close_connection(protocol_failure_code);
        return false;
    }

    return true;
}

bool http3_connection_driver::reject_request_stream(std::uint64_t stream_id) {
    if (rejected_request_count_ >= http3_post_goaway_request_allowance) {
        close_connection(http3_connection_error_code::excessive_load);
        return false;
    }
    auto* transport = wire_->transport();
    if (transport == nullptr) {
        close_connection(protocol_failure_code);
        return false;
    }
    const auto status = wire_->transport()->server().connection(*transport_id_).terminate_bidirectional_stream(stream_id, static_cast<std::uint64_t>(http3_connection_error_code::request_rejected));
    if (status != ruvia::quic_operation_status::accepted &&
        status != ruvia::quic_operation_status::completed &&
        status != ruvia::quic_operation_status::retired) {
        close_connection(protocol_failure_code);
        return false;
    }
    ++rejected_request_count_;
    return true;
}

http3_datagram_receive_status http3_connection_driver::plan_datagram_receive(
    const http3_datagram_view& datagram) const noexcept {
    const auto stream = std::ranges::find(streams_, datagram.stream_id_, &stream_state::id_);
    return plan_http3_datagram_receive(datagram,
        {.local_h3_datagram_ = config_.local_settings_.h3_datagram_,
            .stream_exists_ = stream != streams_.end(),
            .receive_open_ = stream != streams_.end() && !stream->input_terminal_ && !stream->input_reset_,
            .supports_datagrams_ = stream != streams_.end() && stream->tunnel_established_});
}

bool http3_connection_driver::pump_datagrams() {
    auto& quic = wire_->transport()->server().connection(*transport_id_);
    auto& channel = *state_;
    bool progress_value{};
    std::array<std::byte, http3_connection_state::max_datagram_bytes> input{};
    for (std::size_t count = 0; count < http3_connection_state::datagram_capacity; ++count) {
        const auto result_value = quic.read_datagram(input);
        if (result_value.status_ == ruvia::quic_datagram_status::would_block ||
            result_value.status_ == ruvia::quic_datagram_status::unavailable) {
            break;
        }
        progress_value = true;
        if (result_value.status_ == ruvia::quic_datagram_status::too_large) {
            if (!discard_oversized_datagram(quic)) {
                break;
            }
            continue;
        }
        if (result_value.status_ != ruvia::quic_datagram_status::received) {
            continue;
        }
        const auto bytes_value = std::span<const std::byte>(input).first(result_value.size_);
        const auto decoded = decode_http3_datagram({reinterpret_cast<const char*>(bytes_value.data()), bytes_value.size()});
        if ((decoded.index() != 0)) {
            close_connection(static_cast<http3_connection_error_code>(http3_datagram_error_code));
            break;
        }
        const auto planned = plan_datagram_receive(std::get<0>(decoded));
        if (planned == http3_datagram_receive_status::connection_error) {
            close_connection(static_cast<http3_connection_error_code>(http3_datagram_error_code));
            break;
        }
        if (planned == http3_datagram_receive_status::deliver) {
            (void)channel.publish_request_datagram(identity_, std::get<0>(decoded).stream_id_, bytes_value);
        }
    }
    for (std::size_t count = 0; count < http3_connection_state::datagram_capacity; ++count) {
        http3_connection_state::datagram output;
        if (channel.pop_response_datagram(output) != http3_connection_state::status::changed) {
            break;
        }
        progress_value = true;
        if (output.identity_.epoch_ != identity_.epoch_ ||
            output.identity_.connection_generation_ != identity_.connection_generation_) {
            continue;
        }
        (void)quic.write_datagram(output.bytes());
    }
    return progress_value;
}

bool http3_connection_driver::pump_output(bool transport_activity) {
    if (output_ == nullptr) {
        return false;
    }
    if (transport_activity) {
        output_->notify_transport_activity();
    }
    const auto result_value = output_->drive();
    if (result_value.status_ == http3_server_stream_output::status_type::transport_error ||
        result_value.status_ == http3_server_stream_output::status_type::final_size_error ||
        result_value.status_ == http3_server_stream_output::status_type::capacity_exhausted ||
        result_value.status_ == http3_server_stream_output::status_type::unsafe_to_release) {
        close_connection(protocol_failure_code);
    }
    for (auto& stream : streams_) {
        if (!stream.request_stream_ || stream.input_reset_ || stream.write_timeout_notified_) {
            continue;
        }
        const auto info = output_->stream_info(stream.id_);
        if (info && info->timed_out_) {
            // Reset takes precedence over a queued FIN; if FIN was already
            // delivered, input_terminal does not suppress this cancellation.
            stream.write_timeout_notified_ = true;
            note_input_reset(stream.id_);
            stream.frame_tracker_.reset();
            stream.pending_control_ = http3_stream_control{
                .kind_ = http3_stream_control::kind::stream_reset,
                .id_ = {identity_.epoch_,
                    identity_.connection_generation_, stream.id_},
                .value_ = stream.received_bytes_,
                .stream_reset_error_code_ = http3_connection_error_code::request_cancelled,
            };
        }
    }
    if (result_value.accepted_bytes_ != 0 && pending_tunnel_handshakes_ != 0) {
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
            if (!stream.tunnel_established_barrier_) {
                continue;
            }
            const auto info = output_->stream_info(stream.id_);
            if (info && confirm_tunnel_established(stream.id_,
                            info->accepted_wire_bytes_)) {
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
    return result_value.made_progress_ || result_value.needs_reschedule_ || handshake_progress ||
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
                   output_ != nullptr && output_->live_stream_count() == 0) {
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
    const auto state_value = quic.info().state_;
    if (state_value == ruvia::quic_connection_state::failed) {
        force_local_retirement = true;
    }
    if (state_value != ruvia::quic_connection_state::retired &&
        state_value != ruvia::quic_connection_state::closing &&
        state_value != ruvia::quic_connection_state::draining) {
        static constexpr std::string_view graceful_reason = "HTTP/3 drain complete";
        static constexpr std::string_view close_reason = "HTTP/3 connection closed";
        const auto reason = graceful_close_started_ ? graceful_reason : close_reason;
        const auto code = close_code_.value_or(close_code{
            .kind_ = ruvia::quic_close_kind::application,
            .value_ = static_cast<std::uint64_t>(server_shutdown_code)});
        try {
            const auto close = quic.close({
                .kind_ = code.kind_,
                .code_ = code.value_,
                .reason_ = {reason.data(), reason.size()},
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
        quic.info().state_ != ruvia::quic_connection_state::retired) {
        return false;
    }

    // Stop the stream writer before retiring its connection so all borrowed
    // buffer blocks are returned only after HTTP accepts or invalidates writes.
    if (output_ != nullptr) {
        const auto stopped = output_->stop();
        if (stopped.status_ == http3_server_stream_output::status_type::unsafe_to_release) {
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

void http3_connection_driver::close_connection(http3_connection_error_code reason) noexcept {
    start_close({.kind_ = ruvia::quic_close_kind::application, .value_ = static_cast<std::uint64_t>(reason)});
}

void http3_connection_driver::refuse_connection() noexcept {
    start_close({.kind_ = ruvia::quic_close_kind::transport, .value_ = quic_connection_refused});
}

void http3_connection_driver::start_close(close_code code) noexcept {
    if (!transport_id_ || close_started_) {
        return;
    }
    if (graceful_close_started_) {
        graceful_close_abandoned_ = true;
        return;
    }
    close_started_ = true;
    close_code_ = code;
    if (!drain_deadline_) {
        drain_deadline_ = deadline_after(std::chrono::steady_clock::now(), config_.drain_timeout_);
    }
    if (auto* transport = wire_->transport(); transport != nullptr) {
        static constexpr std::string_view close_reason = "HTTP/3 connection closing";
        try {
            (void)transport->server().connection(*transport_id_).close({
                .kind_ = code.kind_,
                .code_ = code.value_,
                .reason_ = {close_reason.data(), close_reason.size()},
            });
        } catch (...) {
            graceful_close_abandoned_ = true;
        }
    }
}

http3_connection_state::intent_execution_result http3_connection_driver::execute_intent(const http3_server_connection::transport_intent_type& intent) noexcept {
    http3_connection_state::intent_execution_result result_value{
        .outcome_ = state_->transport_retired()
                        ? http3_connection_state::execution_outcome::transport_retired
                        : http3_connection_state::execution_outcome::executed};
    if (intent.token_.kind_ == http3_server_connection::transport_intent_kind_type::open_push_stream) {
        result_value.push_stream_.emplace();
        if (state_->transport_retired() || stopping_) {
            result_value.push_stream_->status_ = http3_server_connection::push_stream_open_result_type::status_type::stopped;
        } else if (intent.token_.id_.push_id_ && output_ &&
                   push_streams_.size() < push_streams_.capacity()) {
            try {
                auto* transport = wire_->transport();
                if (!transport) {
                    result_value.push_stream_->status_ = http3_server_connection::push_stream_open_result_type::status_type::stopped;
                } else {
                    auto& quic = wire_->transport()->server().connection(*transport_id_);
                    const auto opened = quic.open_stream(true);
                    if (opened.status_ == ruvia::quic_operation_status::accepted) {
                        const auto registered = output_->register_push_stream(opened.stream_id_, *intent.token_.id_.push_id_);
                        if (registered.status_ == http3_server_stream_output::status_type::accepted) {
                            *result_value.push_stream_ = {.status_ = http3_server_connection::push_stream_open_result_type::status_type::opened,
                                .stream_id_ = opened.stream_id_};
                            push_streams_.push_back({opened.stream_id_, *intent.token_.id_.push_id_});
                        } else {
                            (void)quic.close_stream(opened.stream_id_);
                        }
                    } else if (opened.status_ != ruvia::quic_operation_status::would_block &&
                               opened.status_ != ruvia::quic_operation_status::need_input) {
                        close_connection(protocol_failure_code);
                        result_value.push_stream_->status_ = http3_server_connection::push_stream_open_result_type::status_type::stopped;
                    }
                }
            } catch (...) {
                close_connection(protocol_failure_code);
                result_value.push_stream_->status_ = http3_server_connection::push_stream_open_result_type::status_type::stopped;
            }
        }
    }
    if (!state_->transport_retired()) {
        if (intent.token_.kind_ == http3_server_connection::transport_intent_kind_type::connection_close) {
            close_connection(intent.connection_error_code_.value_or(protocol_failure_code));
        } else if (intent.token_.kind_ == http3_server_connection::transport_intent_kind_type::stream_reset) {
            try {
                terminate_request_stream(intent.token_.id_.stream_id_,
                    static_cast<std::uint64_t>(intent.stream_reset_error_code_));
            } catch (...) {
                close_connection(protocol_failure_code);
            }
            if (!intent.token_.id_.push_id_) {
                stop_request_input(intent.token_.id_.stream_id_);
            }
        }
    }
    return result_value;
}

}  // namespace ruvia::detail
