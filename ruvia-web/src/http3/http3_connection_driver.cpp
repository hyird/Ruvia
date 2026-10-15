#include "http3/http3_connection_driver.h"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>

#include "ruvia/http/http3_local_critical_streams.h"
#include "ruvia/http/http3_peer_streams.h"
#include "ruvia/http/http3_settings.h"

#include "http3/http3_quic_socket_address.h"
#include "http3/http3_server_connection.h"
#include "http3/http3_worker.h"
#include "integration/worker_capabilities.h"
#include "server/http_server_options.h"
#include "server/http_server_options_validation.h"

namespace ruvia::detail {
namespace {

constexpr auto server_shutdown_code = http3_connection_error_code::no_error;
constexpr auto protocol_failure_code = http3_connection_error_code::internal_error;
// RFC 9000 Section 20.1: the server refused to accept a new connection.
constexpr std::uint64_t quic_connection_refused = 0x02;
constexpr std::size_t input_stream_budget = 64;
constexpr std::size_t publication_budget = 64;
constexpr std::size_t datagram_budget = 16;

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

void earliest(std::optional<std::chrono::steady_clock::time_point>& current,
    std::optional<std::chrono::steady_clock::time_point> candidate) noexcept {
    if (candidate && (!current || *candidate < *current)) {
        current = candidate;
    }
}

bool negotiated_h3(std::span<const std::byte> alpn) noexcept {
    const auto expected = std::span(http3_alpn_protocol_list).subspan(1);
    return std::ranges::equal(alpn, expected, [](std::byte left, unsigned char right) {
        return std::to_integer<unsigned char>(left) == right;
    });
}

// RFC 9297 permits dropping an oversized HTTP Datagram, which must remove it.
bool discard_oversized_datagram(ruvia::quic_connection& quic) noexcept {
    std::array<std::byte, ruvia::quic_limits{}.max_datagram_size_> scratch;
    return quic.read_datagram(scratch).status_ == ruvia::quic_datagram_status::received;
}

}  // namespace

http3_connection_driver::http3_connection_driver(http3_worker& worker, http3_connection_driver_config config)
    : worker_(worker),
      config_(config),
      remote_address_(worker.memory_.resource()),
      streams_(worker.memory_.resource()),
      output_(nullptr, pmr_object_deleter<http3_server_stream_output>{worker.memory_.resource()}),
      connection_(nullptr, pmr_object_deleter<http3_server_connection>{worker.memory_.resource()}) {}

http3_connection_driver::~http3_connection_driver() {
    if (token_ || connection_ || joining_) {
        std::terminate();
    }
}

ruvia::quic_connection& http3_connection_driver::quic() {
    return worker_.wire_->transport().server().connection(*token_);
}

// Transport port.

void http3_connection_driver::reset_stream(std::uint64_t stream_id, http3_connection_error_code code) noexcept {
    if (!output_ || !token_) {
        return;
    }
    if (is_http3_request_stream_id(stream_id)) {
        terminate_request_stream(stream_id, static_cast<std::uint64_t>(code));
        if (auto* stream = find_stream(stream_id)) {
            stop_input(*stream);
        }
    } else {
        output_->cancel_stream(stream_id, static_cast<std::uint64_t>(code));
    }
    wake();
}

std::optional<std::uint64_t> http3_connection_driver::open_push_stream(std::uint64_t push_id) noexcept {
    if (!output_ || !token_ || stopping_ || closing()) {
        return std::nullopt;
    }
    const auto opened = output_->open_push_stream(push_id);
    wake();
    return opened;
}

void http3_connection_driver::close_connection(http3_connection_error_code code) noexcept {
    start_close({.kind_ = ruvia::quic_close_kind::application, .value_ = static_cast<std::uint64_t>(code)});
    wake();
}

void http3_connection_driver::send_datagram(std::span<const std::byte> datagram) {
    if (!token_ || closing()) {
        throw std::runtime_error("HTTP Datagram connection is closed");
    }
    // Unreliable by definition: a dropped or oversized datagram is not an error.
    (void)quic().write_datagram(datagram);
    wake();
}

void http3_connection_driver::wake() noexcept {
    worker_.mark_ready(*this);
}

// Worker side.

ruvia::quic_operation_status http3_connection_driver::admit(const ruvia::quic_initial_offer& offer,
    clock_type::time_point now) {
    ruvia::quic_server_admit_result admitted;
    try {
        admitted = worker_.wire_->transport().admit_initial(offer, now);
    } catch (...) {
        // A failed admission consumed its offer: retrying the same Initial
        // fails again and would block every later offer on this worker.
        return ruvia::quic_operation_status::closing;
    }
    if (admitted.status_ == ruvia::quic_operation_status::need_input) {
        return ruvia::quic_operation_status::would_block;
    }
    if (admitted.status_ != ruvia::quic_operation_status::accepted) {
        return admitted.status_;
    }
    try {
        streams_.reserve(ruvia::quic_limits{}.max_streams_);
        worker_.map_token(admitted.connection_, *this);
    } catch (...) {
        worker_.wire_->transport().retire(admitted.connection_);
        return ruvia::quic_operation_status::closing;
    }
    token_ = admitted.connection_;
    handshake_deadline_ = deadline_after(now, config_.handshake_timeout_);
    return ruvia::quic_operation_status::accepted;
}

bool http3_connection_driver::pump(std::span<std::byte> scratch, clock_type::time_point now) {
    if (!token_) {
        return false;
    }
    bool again = false;
    try {
        const auto info = quic().info();
        if (info.state_ == ruvia::quic_connection_state::failed ||
            info.state_ == ruvia::quic_connection_state::retired) {
            transport_failure();
        } else if (!serving() && !closing()) {
            if (stopping_) {
                refuse_connection();
            } else if (handshake_deadline_ && now >= *handshake_deadline_ && !info.quic_handshake_complete_) {
                transport_failure();
            } else {
                again = bind(now);
            }
        }
        if (serving() && !closing()) {
            if (info.state_ == ruvia::quic_connection_state::closing ||
                info.state_ == ruvia::quic_connection_state::draining) {
                close_connection(server_shutdown_code);
            } else {
                again = serve(scratch, now) || again;
            }
        }
        if (output_ && output_->failed()) {
            close_connection(protocol_failure_code);
        }
        if (close_started_ && connection_ && !connection_stop_requested_) {
            connection_stop_requested_ = true;
            (void)connection_->request_stop();
        }
    } catch (const ruvia::quic_error&) {
        transport_failure();
        again = true;
    } catch (...) {
        close_connection(protocol_failure_code);
        again = true;
    }
    try {
        again = retire(now) || again;
    } catch (...) {
        // Retirement must make progress: force the local path.
        graceful_close_abandoned_ = true;
        stopping_ = true;
        again = true;
    }
    return again;
}

void http3_connection_driver::transport_activity() noexcept {
    transport_activity_ = true;
}

void http3_connection_driver::transport_failure() noexcept {
    if (serving()) {
        close_connection(protocol_failure_code);
    } else if (token_) {
        // Not yet an HTTP/3 connection: nothing to drain or join.
        close_started_ = true;
        graceful_close_abandoned_ = true;
    }
}

void http3_connection_driver::request_drain() noexcept {
    if (!token_ || closing()) {
        return;
    }
    draining_ = true;
    if (!serving()) {
        refuse_connection();
        return;
    }
    admission_sealed_ = true;
    (void)announce_goaway();
    wake();
}

void http3_connection_driver::request_stop() noexcept {
    stopping_ = true;
    if (!token_) {
        return;
    }
    if (serving()) {
        close_connection(server_shutdown_code);
    } else {
        refuse_connection();
    }
}

std::optional<http3_connection_driver::clock_type::time_point> http3_connection_driver::deadline() const noexcept {
    std::optional<clock_type::time_point> result;
    if (!token_) {
        return result;
    }
    if (!serving()) {
        earliest(result, handshake_deadline_);
    }
    earliest(result, drain_deadline_);
    if (serving() && !closing()) {
        for (const auto& stream : streams_) {
            earliest(result, input_deadline(stream));
        }
        earliest(result, output_->next_write_deadline());
    }
    return result;
}

task<void> http3_connection_driver::join_retired() {
    std::exception_ptr failure;
    try {
        co_await connection_->join();
    } catch (...) {
        failure = std::current_exception();
    }
    connection_.reset();
    output_.reset();
    worker_.active_connections_.release();
    release_slot();
    joining_ = false;
    worker_.wake();
    if (failure) {
        worker_.report_failure(failure);
    }
}

// Connection work.

bool http3_connection_driver::bind(clock_type::time_point now) {
    auto& quic_connection = quic();
    const auto info = quic_connection.info();
    if (!info.tls_handshake_complete_ || !info.quic_handshake_complete_ || !info.confirmed_ ||
        !negotiated_h3(quic_connection.tls_handshake().info().negotiated_alpn_)) {
        return false;
    }
    const auto peer = to_udp_endpoint(info.peer_address_);
    if (peer.index() != 0) {
        transport_failure();
        return true;
    }
    handshake_deadline_.reset();
    const auto& options = worker_.options_;
    if (stopping_ || worker_.draining_ || (options.server_.max_connections_per_worker_ && worker_.active_connections_.active() >= *options.server_.max_connections_per_worker_)) {
        worker_.refused_connections_.fetch_add(1, std::memory_order_relaxed);
        refuse_connection();
        return true;
    }
    auto* resource = worker_.memory_.resource();
    try {
        remote_address_ = std::get<0>(peer).address().to_string();
        remote_port_ = std::get<0>(peer).port();
        auto prefixes = http3_local_critical_streams::create(config_.local_settings_);
        auto planner = http3_server_request_admission_planner::create(
            {.max_requests_per_connection_ = static_cast<std::uint64_t>(config_.max_requests_per_connection_)});
        if (prefixes.index() != 0 || planner.index() != 0) {
            throw std::runtime_error("HTTP/3 connection protocol setup failed");
        }
        const auto tracked = http3_worker_tracked_stream_capacity(config_.max_requests_per_connection_);
        output_ = make_pmr_object<http3_server_stream_output>(resource, quic_connection, resource,
            http3_server_stream_output_config{
                .max_tracked_streams_ = tracked + http3_server_push_allowance,
                .write_timeout_ = config_.write_timeout_});
        critical_.emplace(std::move(std::get<0>(prefixes)));
        admission_planner_.emplace(std::move(std::get<0>(planner)));
        const auto& settings = config_.local_settings_;
        auto services = worker_.capabilities_.make_context_services(worker_.stop_token_)
                            .with_tls_transport(remote_address_, {}, remote_port_);
        connection_ = make_pmr_object<http3_server_connection>(resource, worker_.routes_, worker_.memory_,
            services, options, *this, worker_.body_budget_,
            http3_server_connection_config{
                .session_ = {
                    .max_buffered_body_bytes_ = options.server_.max_buffered_body_bytes_,
                    .max_stream_body_bytes_ = options.server_.max_stream_body_bytes_,
                    .max_live_streams_ = tracked,
                    .max_buffered_bytes_in_flight_ = http3_worker::body_budget_bytes,
                    .connection_ = {.max_active_streams_ = tracked,
                        .qpack_max_table_capacity_ = static_cast<std::size_t>(settings.qpack_max_table_capacity_),
                        .qpack_blocked_streams_ = static_cast<std::size_t>(settings.qpack_blocked_streams_),
                        .enable_connect_protocol_ = settings.enable_connect_protocol_,
                        .enable_datagrams_ = settings.h3_datagram_},
                    .max_quic_datagram_payload_bytes_ = quic_connection.max_datagram_payload_size(),
                    .inbound_budget_ = options.inbound_budget_,
                    .max_inbound_buffer_bytes_ = options.server_.max_inbound_buffer_bytes_per_connection_,
                },
                .max_tracked_streams_ = tracked,
                .connection_scanner_ = &worker_.scanner_,
                .executor_ = worker_.runtime_.io_context().get_executor(),
            });
    } catch (...) {
        connection_.reset();
        critical_.reset();
        admission_planner_.reset();
        if (output_) {
            output_->detach();
            output_.reset();
        }
        worker_.refused_connections_.fetch_add(1, std::memory_order_relaxed);
        start_close({.kind_ = ruvia::quic_close_kind::application,
            .value_ = static_cast<std::uint64_t>(protocol_failure_code)});
        return true;
    }
    worker_.active_connections_.acquire();
    (void)now;
    return true;
}

bool http3_connection_driver::serve(std::span<std::byte> scratch, clock_type::time_point now) {
    auto& quic_connection = quic();
    const auto critical = critical_->drive(quic_connection, connection_->session(),
        http3_critical_stream_driver::engine_output::send);
    if (critical.status_ == http3_critical_stream_driver::status_type::fatal) {
        close_connection(protocol_failure_code);
        return true;
    }
    bool again = critical.progress_;
    if (goaway_queued_ && critical.status_ == http3_critical_stream_driver::status_type::ready) {
        goaway_bytes_accepted_ = true;
    }
    if (!accept_new_streams(now) || closing()) {
        return true;
    }
    again = pump_streams(scratch, now) || again;
    if (closing()) {
        return true;
    }
    again = pump_datagrams(scratch) || again;
    if (connection_->resume_qpack_input()) {
        again = true;
        for (auto& stream : streams_) {
            latch_request_head(stream);
        }
    }
    if (std::exchange(transport_activity_, false)) {
        connection_->transport_activity();
    }
    again = connection_->publish(publication_budget) || again;
    output_->take_terminated(now, [this](std::uint64_t stream_id) {
        // Write timeout or peer STOP_SENDING: QUIC already terminated the stream.
        if (auto* stream = find_stream(stream_id)) {
            stop_input(*stream);
        }
        deliver_reset(stream_id, http3_connection_error_code::request_cancelled);
    });
    // Engine output produced by this turn's input and publication.
    const auto produced = critical_->drive(quic_connection, connection_->session(),
        http3_critical_stream_driver::engine_output::send);
    if (produced.status_ == http3_critical_stream_driver::status_type::fatal) {
        close_connection(protocol_failure_code);
    }
    again = produced.progress_ || again;
    return again;
}

bool http3_connection_driver::accept_new_streams(clock_type::time_point now) {
    const auto accepted = quic().accept_streams();
    if (accepted.status_ != ruvia::quic_operation_status::accepted &&
        accepted.status_ != ruvia::quic_operation_status::need_input &&
        accepted.status_ != ruvia::quic_operation_status::would_block) {
        close_connection(protocol_failure_code);
        return false;
    }
    for (std::size_t i = 0; i < accepted.size_; ++i) {
        const auto stream_id = accepted.streams_[i].stream_id_;
        if (!accepted.streams_[i].readable_ || streams_.size() >= streams_.capacity()) {
            close_connection(http3_connection_error_code::excessive_load);
            return false;
        }
        if (is_http3_request_stream_id(stream_id)) {
            if (draining_) {
                if (!reject_request_stream(stream_id)) {
                    return false;
                }
                continue;
            }
            const auto decision = admission_planner_->admit(stream_id);
            if (decision.action_ != http3_server_request_admission_action::admit) {
                if (!announce_goaway() || !reject_request_stream(stream_id)) {
                    return false;
                }
                continue;
            }
            ++admitted_request_count_;
        } else if (is_http3_client_unidirectional_stream_id(stream_id)) {
            if (peer_unidirectional_stream_count_ >= http3_peer_unidirectional_stream_allowance) {
                close_connection(http3_connection_error_code::excessive_load);
                return false;
            }
            ++peer_unidirectional_stream_count_;
        } else {
            close_connection(http3_connection_error_code::stream_creation_error);
            return false;
        }
        auto& stream = streams_.emplace_back();
        stream.id_ = stream_id;
        stream.request_stream_ = is_http3_request_stream_id(stream_id);
        stream.last_input_activity_ = now;
        if (stream.request_stream_) {
            if (admitted_request_count_ == config_.max_requests_per_connection_) {
                if (!announce_goaway()) {
                    return false;
                }
                admission_sealed_ = true;
            }
        }
    }
    return true;
}

bool http3_connection_driver::pump_streams(std::span<std::byte> scratch, clock_type::time_point now) {
    auto& quic_connection = quic();
    bool again = false;
    const auto stream_count = streams_.size();
    const auto start = stream_count == 0 ? std::size_t{0} : next_input_stream_index_ % stream_count;
    const auto budget = (std::min)(stream_count, input_stream_budget);
    for (std::size_t processed = 0; processed < budget; ++processed) {
        auto& stream = streams_[(start + processed) % stream_count];
        if (stream.input_terminal_) {
            continue;
        }
        const auto stream_id = stream.id_;
        if (const auto expiry = input_deadline(stream); expiry && now >= *expiry) {
            stop_input(stream);
            terminate_request_stream(stream_id, static_cast<std::uint64_t>(http3_connection_error_code::request_cancelled));
            deliver_reset(stream_id, http3_connection_error_code::request_cancelled);
            if (closing()) {
                return true;
            }
            continue;
        }
        auto limit = scratch.size();
        if (stream.request_stream_) {
            limit = (std::min)(limit, connection_->input_capacity(stream_id));
            if (limit == 0) {
                // Held in QUIC flow control; the connection wakes this driver
                // when its consumer frees capacity.
                continue;
            }
            stream.received_early_data_ = stream.received_early_data_ ||
                                          quic_connection.stream_info(stream_id).received_early_data_;
        }
        const auto read = quic_connection.read_stream(stream_id, scratch.first(limit));
        switch (read.status_) {
            case ruvia::quic_stream_read_status::data: {
                if (read.size_ == 0 || read.size_ > limit) {
                    close_connection(protocol_failure_code);
                    return true;
                }
                const auto bytes = std::span<const std::byte>(scratch.first(read.size_));
                stream.last_input_activity_ = now;
                if (stream.request_stream_) {
                    connection_->accept_data(stream_id, stream.received_early_data_, bytes);
                    latch_request_head(stream);
                } else {
                    connection_->accept_peer_stream_data(stream_id, bytes);
                }
                // A filled read may leave buffered QUIC input without a new event.
                again = again || read.size_ == limit;
                break;
            }
            case ruvia::quic_stream_read_status::would_block:
                break;
            case ruvia::quic_stream_read_status::fin:
                stream.input_fin_ = true;
                stream.input_terminal_ = true;
                connection_->accept_fin(stream_id);
                if (!stream.request_stream_) {
                    const auto closed = quic_connection.close_stream(stream_id);
                    if (closed != ruvia::quic_operation_status::accepted &&
                        closed != ruvia::quic_operation_status::completed &&
                        closed != ruvia::quic_operation_status::retired) {
                        close_connection(protocol_failure_code);
                    }
                } else if (output_->stream_info(stream_id).value_or(http3_server_stream_output::stream_info_type{}).send_fin_accepted_) {
                    (void)quic_connection.retire_completed_stream(stream_id);
                }
                break;
            case ruvia::quic_stream_read_status::reset: {
                stop_input(stream);
                if (stream.request_stream_) {
                    terminate_request_stream(stream_id,
                        static_cast<std::uint64_t>(http3_connection_error_code::request_cancelled));
                } else {
                    (void)quic_connection.close_stream(stream_id);
                }
                deliver_reset(stream_id, static_cast<http3_connection_error_code>(
                                             read.peer_reset_error_code_.value_or(static_cast<std::uint64_t>(
                                                 http3_connection_error_code::request_cancelled))));
                break;
            }
            default:
                close_connection(protocol_failure_code);
                return true;
        }
        if (closing()) {
            return true;
        }
    }
    const auto before = streams_.size();
    // Request streams keep their identity for the connection lifetime: their
    // tunnel and drain state outlive the receive direction.
    std::erase_if(streams_, [](const stream_state& stream) {
        return stream.input_terminal_ && !stream.request_stream_;
    });
    if (streams_.size() != before) {
        next_input_stream_index_ = 0;
    } else if (stream_count != 0) {
        next_input_stream_index_ = (start + budget) % stream_count;
    }
    return again || budget < stream_count;
}

bool http3_connection_driver::pump_datagrams(std::span<std::byte> scratch) {
    auto& quic_connection = quic();
    bool progress = false;
    for (std::size_t count = 0; count < datagram_budget; ++count) {
        const auto received = quic_connection.read_datagram(scratch);
        if (received.status_ == ruvia::quic_datagram_status::would_block ||
            received.status_ == ruvia::quic_datagram_status::unavailable) {
            return progress;
        }
        progress = true;
        if (received.status_ == ruvia::quic_datagram_status::too_large) {
            if (!discard_oversized_datagram(quic_connection)) {
                return progress;
            }
            continue;
        }
        if (received.status_ != ruvia::quic_datagram_status::received) {
            continue;
        }
        const auto decoded = decode_http3_datagram(
            {reinterpret_cast<const char*>(scratch.data()), received.size_});
        if (decoded.index() != 0) {
            close_connection(static_cast<http3_connection_error_code>(http3_datagram_error_code));
            return true;
        }
        const auto planned = plan_datagram_receive(std::get<0>(decoded));
        if (planned == http3_datagram_receive_status::connection_error) {
            close_connection(static_cast<http3_connection_error_code>(http3_datagram_error_code));
            return true;
        }
        if (planned == http3_datagram_receive_status::deliver) {
            connection_->receive_datagram(std::get<0>(decoded));
        }
    }
    // The budget is exhausted; more datagrams may already be queued.
    return true;
}

bool http3_connection_driver::retire(clock_type::time_point now) {
    if (!token_) {
        return false;
    }
    if (goaway_queued_ && !closing()) {
        if (drain_deadline_ && now >= *drain_deadline_) {
            // The deadline also bounds an unsealed admission window.
            close_connection(server_shutdown_code);
        } else if (admission_sealed_ && goaway_bytes_accepted_ && serving() &&
                   output_->live_stream_count() == 0 &&
                   connection_->drain_ready(admitted_request_count_)) {
            graceful_close_started_ = true;
            if (!connection_stop_requested_) {
                connection_stop_requested_ = true;
                (void)connection_->request_stop();
            }
        }
    }
    if (graceful_close_started_ && drain_deadline_ && now >= *drain_deadline_) {
        graceful_close_abandoned_ = true;
    }
    if (!closing()) {
        return false;
    }
    auto& quic_connection = quic();
    bool force = stopping_ || graceful_close_abandoned_ || (close_started_ && drain_deadline_ && now >= *drain_deadline_);
    const auto state = quic_connection.info().state_;
    if (state == ruvia::quic_connection_state::failed) {
        force = true;
    }
    if (state != ruvia::quic_connection_state::retired &&
        state != ruvia::quic_connection_state::closing &&
        state != ruvia::quic_connection_state::draining &&
        state != ruvia::quic_connection_state::failed) {
        static constexpr std::string_view graceful_reason = "HTTP/3 drain complete";
        static constexpr std::string_view close_reason = "HTTP/3 connection closed";
        const auto reason = graceful_close_started_ ? graceful_reason : close_reason;
        const auto code = close_code_.value_or(close_code{
            .kind_ = ruvia::quic_close_kind::application,
            .value_ = static_cast<std::uint64_t>(server_shutdown_code)});
        try {
            const auto closed = quic_connection.close({.kind_ = code.kind_, .code_ = code.value_, .reason_ = {reason.data(), reason.size()}});
            if (closed == ruvia::quic_operation_status::accepted ||
                closed == ruvia::quic_operation_status::completed ||
                closed == ruvia::quic_operation_status::closing) {
                close_flush_pending_ = true;
            } else if (closed != ruvia::quic_operation_status::would_block &&
                       closed != ruvia::quic_operation_status::need_input &&
                       closed != ruvia::quic_operation_status::draining &&
                       closed != ruvia::quic_operation_status::retired) {
                force = true;
            }
        } catch (const ruvia::quic_error&) {
            force = true;
        }
        if (!force) {
            // The closing period ends through QUIC expiry, which wakes this slot.
            return false;
        }
    }
    if (!force && quic_connection.info().state_ != ruvia::quic_connection_state::retired) {
        return false;
    }
    if (std::exchange(close_flush_pending_, false) && worker_.wire_->transport_available() &&
        quic_connection.info().state_ != ruvia::quic_connection_state::failed) {
        // The worker flushes after this turn; that hands any CONNECTION_CLOSE
        // to the channel before the QUIC connection is retired.
        return true;
    }
    release_quic();
    return true;
}

// Helpers.

http3_connection_driver::stream_state* http3_connection_driver::find_stream(std::uint64_t stream_id) noexcept {
    const auto found = std::ranges::find(streams_, stream_id, &stream_state::id_);
    return found == streams_.end() ? nullptr : &*found;
}

bool http3_connection_driver::tunnel_established(std::uint64_t stream_id) const noexcept {
    const auto info = output_ ? output_->stream_info(stream_id) : std::nullopt;
    return info && info->tunnel_established_;
}

std::optional<http3_connection_driver::clock_type::time_point> http3_connection_driver::input_deadline(
    const stream_state& stream) const noexcept {
    if (!stream.request_stream_ || stream.input_terminal_) {
        return std::nullopt;
    }
    const auto timeout = stream.input_phase_ == stream_state::receive_phase::headers
                             ? config_.request_header_timeout_
                         : stream.body_timeout_applies(tunnel_established(stream.id_))
                             ? config_.request_body_timeout_
                             : std::nullopt;
    if (!timeout) {
        return std::nullopt;
    }
    return deadline_after(stream.last_input_activity_, *timeout);
}

http3_datagram_receive_status http3_connection_driver::plan_datagram_receive(
    const http3_datagram_view& datagram) noexcept {
    const auto* stream = find_stream(datagram.stream_id_);
    return plan_http3_datagram_receive(datagram,
        {.local_h3_datagram_ = config_.local_settings_.h3_datagram_,
            .stream_exists_ = stream != nullptr,
            .receive_open_ = stream != nullptr && !stream->input_terminal_ && !stream->input_reset_,
            .supports_datagrams_ = stream != nullptr && tunnel_established(stream->id_)});
}

void http3_connection_driver::latch_request_head(stream_state& stream) noexcept {
    // The session holds a request once its HEADERS section decoded (the
    // request_head event of the feed that completed it, or a QPACK resume);
    // from then on the request-body timeout applies.
    if (stream.request_stream_ && stream.input_phase_ == stream_state::receive_phase::headers &&
        connection_->session().request(stream.id_) != nullptr) {
        stream.input_phase_ = stream_state::receive_phase::body;
    }
}

void http3_connection_driver::terminate_request_stream(std::uint64_t stream_id, std::uint64_t error_code) noexcept {
    if (output_) {
        output_->cancel_stream(stream_id, error_code);
        return;
    }
    try {
        (void)quic().terminate_bidirectional_stream(stream_id, error_code);
    } catch (...) {
        start_close({.kind_ = ruvia::quic_close_kind::application,
            .value_ = static_cast<std::uint64_t>(protocol_failure_code)});
    }
}

void http3_connection_driver::stop_input(stream_state& stream) noexcept {
    stream.input_reset_ = true;
    stream.input_terminal_ = true;
}

void http3_connection_driver::deliver_reset(std::uint64_t stream_id, http3_connection_error_code code) noexcept {
    if (connection_) {
        connection_->accept_reset(stream_id, code);
    }
}

bool http3_connection_driver::announce_goaway() noexcept {
    if (goaway_queued_) {
        return true;
    }
    (void)admission_planner_->announce_goaway();
    if (!critical_->queue_goaway(admission_planner_->goaway_id())) {
        close_connection(protocol_failure_code);
        return false;
    }
    drain_deadline_ = deadline_after(clock_type::now(), config_.drain_timeout_);
    goaway_queued_ = true;
    return true;
}

bool http3_connection_driver::reject_request_stream(std::uint64_t stream_id) {
    if (rejected_request_count_ >= http3_post_goaway_request_allowance) {
        close_connection(http3_connection_error_code::excessive_load);
        return false;
    }
    const auto status = quic().terminate_bidirectional_stream(
        stream_id, static_cast<std::uint64_t>(http3_connection_error_code::request_rejected));
    if (status != ruvia::quic_operation_status::accepted && status != ruvia::quic_operation_status::completed &&
        status != ruvia::quic_operation_status::retired) {
        close_connection(protocol_failure_code);
        return false;
    }
    ++rejected_request_count_;
    return true;
}

void http3_connection_driver::refuse_connection() noexcept {
    start_close({.kind_ = ruvia::quic_close_kind::transport, .value_ = quic_connection_refused});
}

void http3_connection_driver::start_close(close_code code) noexcept {
    if (!token_ || close_started_) {
        return;
    }
    if (graceful_close_started_) {
        graceful_close_abandoned_ = true;
        return;
    }
    close_started_ = true;
    close_code_ = code;
    if (!drain_deadline_) {
        drain_deadline_ = deadline_after(clock_type::now(), config_.drain_timeout_);
    }
    if (!worker_.wire_->transport_available()) {
        return;
    }
    static constexpr std::string_view close_reason = "HTTP/3 connection closing";
    try {
        (void)quic().close({.kind_ = code.kind_, .code_ = code.value_, .reason_ = {close_reason.data(), close_reason.size()}});
        close_flush_pending_ = true;
    } catch (...) {
        graceful_close_abandoned_ = true;
    }
}

void http3_connection_driver::release_quic() noexcept {
    if (output_) {
        output_->detach();
    }
    critical_.reset();
    worker_.unmap_token(*token_);
    worker_.wire_->transport().retire(*token_);
    token_.reset();
    if (connection_) {
        connection_->transport_retired();
        joining_ = true;
        try {
            worker_.spawn_join(*this);
        } catch (...) {
            // Without a join the HTTP connection cannot be retired safely.
            std::terminate();
        }
        return;
    }
    output_.reset();
    release_slot();
}

void http3_connection_driver::release_slot() noexcept {
    handshake_deadline_.reset();
    drain_deadline_.reset();
    remote_address_.clear();
    remote_port_ = 0;
    streams_.clear();
    next_input_stream_index_ = 0;
    critical_.reset();
    admission_planner_.reset();
    close_code_.reset();
    admitted_request_count_ = 0;
    peer_unidirectional_stream_count_ = 0;
    rejected_request_count_ = 0;
    admission_sealed_ = false;
    draining_ = false;
    goaway_queued_ = false;
    goaway_bytes_accepted_ = false;
    graceful_close_started_ = false;
    graceful_close_abandoned_ = false;
    close_started_ = false;
    close_flush_pending_ = false;
    connection_stop_requested_ = false;
    transport_activity_ = false;
}

}  // namespace ruvia::detail
