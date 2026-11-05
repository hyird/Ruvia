#include "http2/http2_connection.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

#include "ruvia/http/http_pmr_string.h"

#include "http2/http2_frame_codec.h"
#include "http2/http2_frame_payload.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_window_update.h"
#include "util/geometric_reserve.h"

namespace ruvia::detail {

namespace {
constexpr std::string_view http2_drain_debug = "connection draining";
// Opaque payloads of the locally originated PINGs (RFC 9113 §6.7 echoes them
// unchanged); distinct values tell the graceful-drain round trip from liveness.
constexpr std::string_view http2_drain_ping_payload = "ruviadrn";
constexpr std::string_view http2_liveness_ping_payload = "ruvialiv";
static_assert(http2_drain_ping_payload.size() == 8 && http2_liveness_ping_payload.size() == 8);

[[nodiscard]] std::size_t http2_goaway_encoded_bytes(std::string_view debug) {
    constexpr std::size_t goaway_fixed_payload_bytes = 8;
    constexpr std::size_t fixed_bytes = http2_frame_header_bytes + goaway_fixed_payload_bytes;
    if (debug.size() > std::numeric_limits<std::size_t>::max() - fixed_bytes) {
        throw std::length_error("HTTP/2 GOAWAY output size overflow");
    }
    return fixed_bytes + debug.size();
}
}  // namespace

bool http2_connection::admit_undrained_control_frame() {
    if (undrained_control_frames_ < max_undrained_control_frames) {
        return true;
    }
    append_goaway(http2_error_code::enhance_your_calm, "excessive control frames");
    return false;
}

http2_connection::http2_connection(std::pmr::memory_resource* resource, http2_role role, bool enable_push,
    bool receive_origin_advertisements, bool receive_frame_metadata)
    : resource_(resource),
      input_(std::string_view{}, resource),
      local_header_block_(std::string_view{}, resource),
      output_(resource),
      streams_(resource),
      priorities_(resource),
      decoder_(resource),
      peer_settings_(role),
      events_(std::make_move_iterator(static_cast<http2_event*>(nullptr)),
          std::make_move_iterator(static_cast<http2_event*>(nullptr)), resource),
      role_(role),
      connection_send_window_(http2_default_initial_window_size),
      connection_receive_window_(static_cast<std::int32_t>(http2_local_settings::initial_window_size)) {
    enable_push_ = role == http2_role::client && enable_push;
    receive_origin_advertisements_ = role == http2_role::client && receive_origin_advertisements;
    receive_frame_metadata_ = receive_frame_metadata;
    decoder_.set_max_dynamic_table_size(http2_local_settings::header_table_size);
}

// --- outbound byte buffer (batched writes) ------------------------------------

http2_output_batch_result http2_connection::take_output_batch(std::size_t max_bytes,
    std::pmr::string& into, http2_data_output_observer_type observer, void* observer_context) {
    const auto result_value = output_.take_batch(max_bytes, into, observer, observer_context);
    if (result_value.status_ == http2_output_batch_status::taken && !output_.wants_write()) {
        undrained_control_frames_ = 0;
    }
    return result_value;
}

// --- event queue --------------------------------------------------------------

http2_event* http2_connection::peek_event() & noexcept {
    if (event_offset_ < events_.size()) {
        return &events_[event_offset_];
    }
    events_.clear();
    event_offset_ = 0;
    return nullptr;
}

bool http2_connection::has_pending_events(std::uint32_t stream_id) const noexcept {
    // A removed stream has no storage left for a queued event to read.
    const auto* stream = streams_.find(stream_id);
    return stream != nullptr && stream->has_pending_events();
}

void http2_connection::consume_event() noexcept {
    if (event_offset_ >= events_.size()) {
        std::terminate();
    }
    // Snapshot the ids first: a lease retry may publish events and reallocate the queue.
    std::array<std::uint32_t, 2> stream_ids{};
    std::size_t stream_count = 0;
    events_[event_offset_++].for_each_referenced_stream([&](std::uint32_t stream_id) noexcept {
        stream_ids[stream_count++] = stream_id;
    });
    // Only the consumed event's streams can newly satisfy a deferred lease release.
    for (std::size_t index = 0; index < stream_count; ++index) {
        if (auto* stream = streams_.find(stream_ids[index])) {
            stream->remove_pending_event();
        }
        retry_deferred_stream_lease(stream_ids[index]);
    }
}

void http2_connection::publish_event(http2_event event) {
    events_.push_back(std::move(event));
    events_.back().for_each_referenced_stream([this](std::uint32_t stream_id) noexcept {
        if (auto* stream = streams_.find(stream_id)) {
            stream->add_pending_event();
        }
    });
}

void http2_connection::reserve_event_slots(std::size_t count) {
    if (count > events_.max_size() - events_.size()) {
        throw std::length_error("HTTP/2 event queue size overflow");
    }
    // Reserve before state changes, without forcing a reallocation for every frame.
    reserve_geometric(events_, events_.size() + count);
}

void http2_connection::append_goaway(http2_error_code error, std::string_view debug) {
    // RFC 9113 §6.8: "Endpoints MUST NOT increase the value they send in the last
    // stream identifier". last_stream_id_ doubles as the idle-stream high-water mark
    // (§5.1.1), so it keeps climbing for streams a graceful drain already refused --
    // those are exactly the ids the peer was told it may retry elsewhere. Clamp to the
    // advertised drain boundary; read it before fail() replaces the drain state.
    auto advertised = last_stream_id_;
    if (const auto* drain = local_connection_state_.graceful_drain()) {
        advertised = std::min(advertised, drain->last_stream_id());
    }
    // A fatal transition is only observable together with its GOAWAY. Preflight the
    // complete frame so a throwing PMR resource cannot leave the connection marked
    // failed while the required terminal bytes were never queued.
    output_.reserve_segments_additional(1);
    output_.reserve_additional(http2_goaway_encoded_bytes(debug));
    local_connection_state_.fail(error);
    output_.append_goaway_frame(advertised, error, debug);
}

void http2_connection::begin_drain() {
    // Graceful drain (RFC 9113 §6.8) WITHOUT a connection error: established streams
    // keep running, and HEADERS for a stream above the advertised id are refused in
    // process_headers. A server first advertises 2^31-1 and keeps admitting the
    // streams the client may already have in flight; the final boundary follows the
    // ACK of a PING queued right behind that GOAWAY, i.e. one round trip later.
    if (local_connection_state_.open() == nullptr) {
        return;
    }
    if (role_ != http2_role::server) {
        begin_final_drain();
        return;
    }
    // Keep the lifecycle transition and its frames as one externally visible
    // operation. The reservation is made while the state is still open, so a
    // failed allocation leaves begin_drain() retryable.
    output_.reserve_segments_additional(2);
    output_.reserve_additional(http2_goaway_encoded_bytes(http2_drain_debug) + http2_frame_header_bytes +
                               http2_drain_ping_payload.size());
    // A client that ignores the first GOAWAY and withholds the ACK still cannot
    // extend the round trip indefinitely: one concurrency window of further streams
    // is admitted, then the boundary becomes final. finish_drain() bounds it in time.
    const auto stream_budget = std::uint32_t{2} * http2_local_settings::max_concurrent_streams;
    const auto round_trip_limit =
        last_stream_id_ > http2_max_stream_id - stream_budget ? http2_max_stream_id : last_stream_id_ + stream_budget;
    if (!local_connection_state_.begin_round_trip_drain(http2_max_stream_id, round_trip_limit)) {
        return;
    }
    output_.append_goaway_frame(http2_max_stream_id, http2_error_code::no_error, http2_drain_debug);
    output_.append_frame(http2_frame_type::ping, 0, 0, http2_drain_ping_payload);
}

void http2_connection::finish_drain() {
    if (local_connection_state_.open() != nullptr) {
        begin_final_drain();
        return;
    }
    finish_drain_round_trip();
}

void http2_connection::begin_final_drain() {
    // Reserve while still open so a failed allocation leaves the transition retryable.
    output_.reserve_segments_additional(1);
    output_.reserve_additional(http2_goaway_encoded_bytes(http2_drain_debug));
    if (local_connection_state_.begin_graceful_drain(last_stream_id_)) {
        output_.append_goaway_frame(last_stream_id_, http2_error_code::no_error, http2_drain_debug);
    }
}

void http2_connection::finish_drain_round_trip() {
    const auto* drain = local_connection_state_.graceful_drain();
    if (drain == nullptr || !drain->awaiting_round_trip()) {
        return;
    }
    output_.reserve_segments_additional(1);
    output_.reserve_additional(http2_goaway_encoded_bytes(http2_drain_debug));
    if (local_connection_state_.finish_round_trip_drain(last_stream_id_)) {
        output_.append_goaway_frame(last_stream_id_, http2_error_code::no_error, http2_drain_debug);
    }
}

bool http2_connection::apply_settings_payload(std::string_view payload_value) {
    if (!http2_settings_payload_size_valid(payload_value)) {
        append_goaway(http2_error_code::frame_size_error, "invalid SETTINGS size");
        return false;
    }

    // SETTINGS is one protocol transaction. Validate and apply every entry to a
    // detached candidate first; an invalid later entry must not leave an earlier
    // valid entry visible in the live peer model before the fatal GOAWAY is queued.
    http2_peer_settings candidate_value = peer_settings_;
    bool encoder_table_size_reduction = false;
    const auto initial_window_baseline =
        static_cast<std::int64_t>(peer_settings_.initial_window_size());
    std::int64_t minimum_window_delta = 0;
    std::int64_t maximum_window_delta = 0;
    const http2_received_settings settings(payload_value);
    for (std::size_t index = 0; index < settings.size(); ++index) {
        const auto entry_value = settings.at(index);
        if (const auto error = candidate_value.apply(entry_value.id_, entry_value.value_)) {
            append_goaway(http2_peer_setting_error_code(*error),
                http2_peer_setting_error_message(*error));
            return false;
        }
        if (entry_value.id_ == http2_setting_id::initial_window_size) {
            const auto delta = static_cast<std::int64_t>(entry_value.value_) -
                               initial_window_baseline;
            minimum_window_delta = (std::min)(minimum_window_delta, delta);
            maximum_window_delta = (std::max)(maximum_window_delta, delta);
        }
        if (entry_value.id_ == http2_setting_id::header_table_size &&
            entry_value.value_ < encoder_dynamic_table_size_) {
            encoder_table_size_reduction = true;
        }
    }

    const auto initial_window_delta = static_cast<std::int64_t>(candidate_value.initial_window_size()) -
                                      initial_window_baseline;
    // Validate every intermediate window as well as the final setting before
    // publishing the candidate or changing any active stream.
    if (!streams_.apply_send_window_delta(
            initial_window_delta, minimum_window_delta, maximum_window_delta)) {
        append_goaway(http2_error_code::flow_control_error, "stream window overflow");
        return false;
    }
    if (encoder_table_size_reduction) {
        // RFC 9113 §4.3.1 requires the next field block after our SETTINGS ACK
        // to begin with a conformant table-size update. This encoder never uses
        // dynamic entries, so permanently selecting zero is both exact and avoids
        // carrying a fictitious compression capacity through later SETTINGS.
        encoder_dynamic_table_size_ = 0;
        encoder_table_size_update_pending_ = true;
    }
    candidate_value.complete_frame();
    peer_settings_ = candidate_value;
    return true;
}

bool http2_connection::process_settings(const http2_frame_header& header_value, std::string_view payload_value) {
    if (header_value.stream_id_ != 0) {
        append_goaway(http2_error_code::protocol_error, "SETTINGS stream id must be zero");
        return false;
    }
    if ((header_value.flags_ & http2_flag_ack) != 0) {
        if (!payload_value.empty()) {
            append_goaway(http2_error_code::frame_size_error, "SETTINGS ack payload");
            return false;
        }
        if (preface_phase_ == preface_phase_type::awaiting_peer_settings) {
            // Each peer preface must start with a (possibly empty) non-ACK SETTINGS
            // frame (RFC 9113 §3.4); an ACK alone does not satisfy either role.
            append_goaway(http2_error_code::protocol_error, "SETTINGS ACK before SETTINGS");
            return false;
        }
        return true;
    }
    // Check the control-output budget before applying this frame, so the frame
    // that would exceed it consumes no SETTINGS state before its fatal GOAWAY.
    if (!admit_undrained_control_frame()) {
        return false;
    }
    // The ACK is part of the SETTINGS transaction. Reserve it before changing
    // peer settings or stream windows so a throwing PMR resource leaves the
    // incoming frame wholly retryable.
    const auto output_checkpoint = output_.checkpoint();
    const auto previous_settings = peer_settings_;
    const auto previous_encoder_dynamic_table_size = encoder_dynamic_table_size_;
    const auto previous_encoder_table_size_update_pending = encoder_table_size_update_pending_;
    const auto previous_preface_phase = preface_phase_;
    const auto previous_initial_window_size = peer_settings_.initial_window_size();
    bool settings_applied = false;
    try {
        output_.reserve_additional(http2_frame_header_bytes);
        if (receive_frame_metadata_) {
            // Nothing below queues another event before this frame's own.
            reserve_event_slots(1);
        }
        if (!apply_settings_payload(payload_value)) {
            return false;
        }
        settings_applied = true;
        if (preface_phase_ == preface_phase_type::awaiting_peer_settings) {
            preface_phase_ = preface_phase_type::ready;
        }
        output_.append_frame(http2_frame_type::settings, http2_flag_ack, 0, {});
        if (receive_frame_metadata_) {
            // Last, after every throwing step: the view borrows the accepted input.
            publish_event(http2_event(http2_received_settings(payload_value)));
        }
        ++undrained_control_frames_;
        return true;
    } catch (...) {
        if (settings_applied) {
            const auto applied_initial_window_delta =
                static_cast<std::int64_t>(peer_settings_.initial_window_size()) -
                static_cast<std::int64_t>(previous_initial_window_size);
            const auto rollback_delta = -applied_initial_window_delta;
            if (!streams_.apply_send_window_delta(rollback_delta, rollback_delta, rollback_delta)) {
                std::terminate();
            }
            peer_settings_ = previous_settings;
            encoder_dynamic_table_size_ = previous_encoder_dynamic_table_size;
            encoder_table_size_update_pending_ = previous_encoder_table_size_update_pending;
            preface_phase_ = previous_preface_phase;
            output_.rollback_to(output_checkpoint);
        }
        throw;
    }
}

bool http2_connection::process_priority(const http2_frame_header& header_value, std::string_view payload_value) {
    if (header_value.stream_id_ == 0) {
        append_goaway(http2_error_code::protocol_error, "PRIORITY stream id must be nonzero");
        return false;
    }
    if (payload_value.size() != 5) {
        auto* const stream = streams_.find(header_value.stream_id_);
        if (stream == nullptr || http2_stream_is_closed(*stream)) {
            // PRIORITY is allowed in every state, but RST_STREAM is not legal
            // on an idle or closed stream. Promote its mandatory stream
            // FRAME_SIZE_ERROR to the connection instead.
            append_goaway(http2_error_code::frame_size_error, "invalid PRIORITY without active stream");
            return false;
        }
        // RFC 9113 section 6.3 makes malformed PRIORITY length a stream error,
        // unlike most fixed-size connection-control frames. Do not terminate
        // unrelated multiplexed streams for one bad advisory frame.
        return reset_stream_for_peer_error(header_value.stream_id_, http2_error_code::frame_size_error);
    }
    // RFC 9113 §6.3：已废弃的优先级树不参与协议状态或调度，元数据事件也不新增校验。
    if (receive_frame_metadata_) {
        reserve_event_slots(1);
        publish_event(http2_event(http2_decode_priority_fields(
            header_value.stream_id_, reinterpret_cast<const unsigned char*>(payload_value.data()))));
    }
    return true;
}

bool http2_connection::process_goaway(const http2_frame_header& header_value, std::string_view payload_value) {
    // RFC 9113 §6.8 gives these two malformations distinct codes: a nonzero stream id is
    // a PROTOCOL_ERROR, while a payload short of the 8-octet fixed fields is a
    // FRAME_SIZE_ERROR. Both are connection errors, but conformance suites read the code.
    if (header_value.stream_id_ != 0) {
        append_goaway(http2_error_code::protocol_error, "GOAWAY stream id must be zero");
        return false;
    }
    if (payload_value.size() < 8) {
        append_goaway(http2_error_code::frame_size_error, "invalid GOAWAY size");
        return false;
    }

    const http2_goaway_event goaway(
        http2_read31(reinterpret_cast<const unsigned char*>(payload_value.data())),
        static_cast<http2_error_code>(
            http2_read32(reinterpret_cast<const unsigned char*>(payload_value.data() + 4))));
    if (peer_goaway_ && goaway.last_stream_id() > peer_goaway_->last_stream_id()) {
        // RFC 9113 §6.8: increasing this value can make an already retried request
        // ambiguous, so the peer is not allowed to widen it on a later GOAWAY.
        append_goaway(http2_error_code::protocol_error, "GOAWAY last stream id increased");
        return false;
    }

    // A GOAWAY boundary can cover more live streams than the concurrency budget:
    // reserved push promises do not occupy a concurrency slot (RFC 9113 Sections
    // 5.1.2 and 8.4.2) yet remain open and must be closed here.
    std::array<std::uint32_t, http2_stream_table::max_streams> unprocessed_stream_ids;
    std::size_t unprocessed_count = 0;
    if (role_ == http2_role::client) {
        bool response_started_above_last = false;
        streams_.for_each([&](http2_stream_state& stream) {
            if ((stream.id() & 1U) == 0 || stream.id() <= goaway.last_stream_id() || stream.is_aborted()) {
                return;
            }
            // A response head proves that the peer acted on this request. Claiming
            // otherwise would make replay unsafe, so reject the contradictory GOAWAY.
            if (http2_remote_final_head_decoded(stream) || stream.has_informational_response()) {
                response_started_above_last = true;
                return;
            }
            unprocessed_stream_ids[unprocessed_count++] = stream.id();
        });
        if (response_started_above_last) {
            append_goaway(http2_error_code::protocol_error, "GOAWAY excludes a started response");
            return false;
        }
        std::ranges::sort(std::span(unprocessed_stream_ids).first(unprocessed_count));
    } else {
        streams_.for_each([&](http2_stream_state& stream) {
            if ((stream.id() & 1U) == 0 && stream.id() > goaway.last_stream_id() && !http2_stream_is_closed(stream)) {
                unprocessed_stream_ids[unprocessed_count++] = stream.id();
            }
        });
    }

    // Reserve every event that this frame can publish before changing peer or local
    // lifecycle state. The client may add one request-unprocessed event per stream
    // below; without this preflight an allocator failure after peer_goaway_ was set
    // would leave a half-published GOAWAY transaction.
    const auto event_count = std::size_t{1} + unprocessed_count;
    reserve_event_slots(event_count);

    // A valid peer GOAWAY is graceful shutdown state, not a local connection error.
    // Send our directional GOAWAY through the same idempotent drain path before
    // publishing the peer event. Its sender opens no further streams (RFC 9113 §6.8),
    // so a server needs no round trip: the boundary is final at once, also ending
    // a round trip already in progress. finish_drain() reserves its complete output
    // first, so a throwing resource leaves both lifecycle sides retryable.
    finish_drain();
    peer_goaway_ = goaway;
    publish_event(http2_event(goaway));

    if (role_ == http2_role::server) {
        for (std::size_t i = 0; i < unprocessed_count; ++i) {
            (void)close_stream_impl(unprocessed_stream_ids[i], http2_stream_close_source::peer_goaway,
                goaway.error(), close_notification_type::emit_event);
        }
        return true;
    }

    // Higher locally initiated streams were never processed and can be replayed on a
    // new connection. Close them inside the protocol core so flow-control debt,
    // stream-table storage, and peer concurrency slots cannot leak.
    for (std::size_t i = 0; i < unprocessed_count; ++i) {
        const auto stream_id = unprocessed_stream_ids[i];
        if (!close_stream_impl(stream_id, http2_stream_close_source::peer_goaway, goaway.error(),
                close_notification_type::owner_already_knows)) {
            continue;
        }
        publish_event(http2_event(http2_request_unprocessed_event(stream_id)));
    }
    return true;
}

bool http2_connection::process_ping(const http2_frame_header& header_value, std::string_view payload_value) {
    if (payload_value.size() != 8) {
        append_goaway(http2_error_code::frame_size_error, "invalid PING");
        return false;
    }
    if (header_value.stream_id_ != 0) {
        append_goaway(http2_error_code::protocol_error, "PING stream id must be zero");
        return false;
    }
    if ((header_value.flags_ & http2_flag_ack) != 0) {
        // The drain PING's ACK completes the round trip that in-flight stream
        // creation was given (RFC 9113 §6.8). Any received frame, this ACK
        // included, already proved liveness for submit_ping().
        if (payload_value == http2_drain_ping_payload) {
            finish_drain_round_trip();
        } else if (payload_value == http2_liveness_ping_payload) {
            liveness_ping_outstanding_ = false;
        }
        return true;
    }
    if (!admit_undrained_control_frame()) {
        return false;
    }
    // Reserve before either side effect so an allocation failure does not
    // consume the peer's retryable PING or make the budget lie about the output.
    output_.reserve_additional(http2_frame_header_bytes + payload_value.size());
    output_.append_frame(http2_frame_type::ping, http2_flag_ack, 0, payload_value);  // echo back
    ++undrained_control_frames_;
    return true;
}

http2_submit_status http2_connection::submit_ping() {
    if (local_connection_state_.fatal_failure() != nullptr) {
        return http2_submit_status::closed;
    }
    if (liveness_ping_outstanding_) {
        return http2_submit_status::accepted;
    }
    output_.append_frame(http2_frame_type::ping, 0, 0, http2_liveness_ping_payload);
    liveness_ping_outstanding_ = true;
    return http2_submit_status::accepted;
}

http2_submit_status http2_connection::submit_priority_update(std::uint32_t stream_id, http_priority_fields fields_value) {
    if (role_ != http2_role::client || local_connection_state_.open() == nullptr || !stream_id ||
        stream_id > http2_max_stream_id || ((stream_id & 1) == 0 && is_idle_stream_id(stream_id))) {
        return http2_submit_status::invalid_state;
    }
    std::array<char, 32> encoded{};
    const auto size = encode_http2_priority_update(encoded, stream_id, fields_value);
    if ((size.index() != 0)) {
        return http2_submit_status::invalid_message;
    }
    output_.append_frame(http2_frame_type::priority_update, 0, 0,
        std::string_view(encoded.data() + http2_frame_header_bytes, std::get<0>(size) - http2_frame_header_bytes));
    return http2_submit_status::accepted;
}

bool http2_connection::process_priority_update(const http2_frame_header& header_value, std::string_view payload_value) {
    if (role_ != http2_role::server) {
        append_goaway(http2_error_code::protocol_error, "server must not send PRIORITY_UPDATE");
        return false;
    }
    if (header_value.stream_id_ != 0) {
        append_goaway(http2_error_code::protocol_error, "PRIORITY_UPDATE frame stream id must be 0");
        return false;
    }
    if (payload_value.size() < 4) {
        append_goaway(http2_error_code::frame_size_error, "PRIORITY_UPDATE frame payload too short");
        return false;
    }
    const auto id = http2_read31(reinterpret_cast<const unsigned char*>(payload_value.data()));
    if (!id || ((id & 1) == 0 && is_idle_stream_id(id))) {
        append_goaway(http2_error_code::protocol_error, "invalid prioritized stream");
        return false;
    }
    const auto update = decode_http2_priority_update(std::span(payload_value.data(), payload_value.size()));
    if ((update.index() != 0)) {
        return true;  // A malformed Priority field does not prioritize a stream.
    }
    const bool new_entry = !priorities_.contains(id);
    if (new_entry) {
        // Entries are removed when their stream closes. An id the peer skipped
        // never opens, so its entry becomes unreachable once a higher stream
        // opens; drop those only when the map would otherwise grow.
        std::erase_if(priorities_, [&](const auto& entry_value) {
            return !is_idle_stream_id(entry_value.first) && streams_.find(entry_value.first) == nullptr;
        });
    }
    const bool new_idle = new_entry && is_idle_stream_id(id);
    if (new_idle) {
        // RFC 9218 §7.1: retained idle priorities plus active peer streams stay
        // within the advertised concurrency limit.
        std::size_t idle = 0;
        for (const auto& entry : priorities_) {
            if (is_idle_stream_id(entry.first)) {
                ++idle;
            }
        }
        if (idle + 1 + open_streams_initiated_by(http2_role::client) > http2_local_settings::max_concurrent_streams) {
            append_goaway(http2_error_code::protocol_error, "too many prioritized idle streams");
            return false;
        }
    }
    http2_stream_state* active_stream = nullptr;
    if (!is_idle_stream_id(id)) {
        active_stream = streams_.find(id);
        if (!active_stream || http2_stream_is_closed(*active_stream) ||
            active_stream->local_send().end_stream_committed() != nullptr) {
            return true;
        }
    }
    reserve_event_slots(1);
    const auto fields = std::get<0>(update).fields_;
    priorities_.insert_or_assign(id, fields);
    if (active_stream != nullptr) {
        active_stream->set_priority_update(fields);
    }
    publish_event(http2_event(std::get<0>(update)));
    return true;
}

bool http2_connection::process_frame(const http2_frame_header& header_value, std::string_view payload_value) {
    if (preface_phase_ == preface_phase_type::awaiting_peer_settings &&
        header_value.type_ != static_cast<std::uint8_t>(http2_frame_type::settings)) {
        append_goaway(http2_error_code::protocol_error, "first frame must be SETTINGS");
        return false;
    }
    if (!header_continuation_.expects_frame_type(header_value.type_)) {
        append_goaway(http2_error_code::protocol_error, "expected CONTINUATION");
        return false;
    }
    const auto type = static_cast<http2_frame_type>(header_value.type_);
    const bool known_frame = header_value.type_ <= 9 ||
                             (role_ == http2_role::client && type == http2_frame_type::alternative_service);
    if (auto* stream = streams_.find(header_value.stream_id_); stream && !stream->is_aborted()) {
        if (known_frame) {
            const auto reservation = stream->push_reservation();
            const bool permitted = type == http2_frame_type::rst_stream || type == http2_frame_type::priority ||
                                   (reservation == http2_push_reservation::local && type == http2_frame_type::window_update) ||
                                   (reservation == http2_push_reservation::remote &&
                                       (type == http2_frame_type::headers || type == http2_frame_type::continuation));
            if (reservation != http2_push_reservation::none && !permitted) {
                append_goaway(http2_error_code::protocol_error, "frame on reserved push stream");
                return false;
            }
        }
    }
    switch (static_cast<http2_frame_type>(header_value.type_)) {
        case http2_frame_type::settings:
            return process_settings(header_value, payload_value);
        case http2_frame_type::ping:
            return process_ping(header_value, payload_value);
        case http2_frame_type::window_update:
            return process_window_update(header_value, payload_value);
        case http2_frame_type::rst_stream:
            return process_rst_stream(header_value, payload_value);
        case http2_frame_type::origin:
        case http2_frame_type::alternative_service:
            return process_advertisement(header_value, payload_value);
        case http2_frame_type::priority_update:
            return process_priority_update(header_value, payload_value);
        case http2_frame_type::priority:
            return process_priority(header_value, payload_value);
        case http2_frame_type::headers:
            return process_headers(header_value, payload_value);
        case http2_frame_type::continuation:
            return process_continuation(header_value, payload_value);
        case http2_frame_type::data:
            return process_data(header_value, payload_value);
        case http2_frame_type::goaway:
            return process_goaway(header_value, payload_value);
        case http2_frame_type::push_promise:
            return process_push_promise(header_value, payload_value);
        default:
            return true;
    }
}

bool http2_connection::consume_frames(std::string_view buffer, std::size_t& offset) {
    for (;;) {
        const std::size_t available = buffer.size() - offset;
        if (available < http2_frame_header_bytes) {
            break;
        }
        const auto header_value = http2_parse_frame_header(buffer.substr(offset, http2_frame_header_bytes));
        if (header_value.length_ > http2_local_settings::max_frame_size) {
            append_goaway(http2_error_code::frame_size_error, "frame too large");
            return false;
        }
        if (available < http2_frame_header_bytes + header_value.length_) {
            break;  // partial frame; wait for the next feed
        }
        const auto payload_value = buffer.substr(offset + http2_frame_header_bytes, header_value.length_);
        if (!process_frame(header_value, payload_value)) {
            return false;
        }
        offset += http2_frame_header_bytes + header_value.length_;
        if (local_connection_state_.fatal_failure() != nullptr) {
            break;
        }
    }
    return true;
}

std::optional<http2_feed_result> http2_connection::consume_client_magic(
    std::string_view buffer, std::size_t& offset) {
    // Server mode: the 24-byte client connection preface precedes the first frame
    // (RFC 9113 §3.4). Consume and validate it before any frame parsing.
    if (preface_phase_ != preface_phase_type::awaiting_client_magic) {
        return std::nullopt;
    }
    if (buffer.size() - offset < http2_client_preface.size()) {
        return http2_feed_result::need_input;
    }
    if (buffer.substr(offset, http2_client_preface.size()) != http2_client_preface) {
        append_goaway(http2_error_code::protocol_error, "invalid connection preface");
        return http2_feed_result::protocol_failure;
    }
    offset += http2_client_preface.size();
    preface_phase_ = preface_phase_type::awaiting_peer_settings;
    return std::nullopt;
}

http2_feed_result http2_connection::dispatch_input(std::string_view buffer, std::size_t& offset) {
    if (const auto magic_result = consume_client_magic(buffer, offset)) {
        return *magic_result;
    }
    if (!consume_frames(buffer, offset) || local_connection_state_.fatal_failure() != nullptr) {
        return http2_feed_result::protocol_failure;
    }
    return offset < buffer.size() ? http2_feed_result::need_input : http2_feed_result::accepted;
}

http2_feed_result http2_connection::feed(std::string_view in) {
    // The facade begins the connection before it offers any input, so a core
    // fed before begin_connection() is a driver bug, not a protocol outcome.
    if (preface_phase_ == preface_phase_type::not_started) {
        std::terminate();
    }
    // Event delivery is part of input ownership, not a best-effort side channel. An
    // unread event may carry a zero-copy view into the previous input, so accepting
    // another span here would both discard the event and invalidate its bytes.
    if (event_offset_ < events_.size()) {
        return http2_feed_result::events_pending;
    }
    if (local_connection_state_.fatal_failure() != nullptr) {
        return http2_feed_result::protocol_failure;
    }

    // Reclaim the prefix consumed by the PREVIOUS feed now, at the start of this one --
    // not at the end of that feed. A message_body_chunk event carries a view INTO input_
    // (or into the caller's `in` on the fast path), and reclaiming shifts the buffer;
    // deferring the reclaim keeps those views valid until this next feed, matching the
    // documented contract. All prior events have been pulled (enforced above), so the
    // exhausted queue and its now-stale views can be reset. A retry keeps the prefix
    // and cursor intact until the retained uncommitted suffix has been dispatched.
    const auto retry_offset = std::exchange(retry_offset_, std::nullopt);
    if (!retry_offset && input_offset_ > 0) {
        if (input_offset_ == input_.size()) {
            http_clear_pmr_string_retaining_small(input_);
            input_offset_ = 0;
        } else if (input_offset_ >= 4096 && input_offset_ * 2 >= input_.size()) {
            input_.erase(0, input_offset_);
            input_offset_ = 0;
        }
    }
    events_.clear();
    event_offset_ = 0;

    if (input_.empty()) {
        // FAST PATH: parse over the caller's bytes; only an incomplete tail is owned.
        std::size_t offset = retry_offset ? std::min(*retry_offset, in.size()) : std::size_t{0};
        try {
            const auto result_value = dispatch_input(in, offset);
            if (result_value == http2_feed_result::need_input) {
                input_.append(in.data() + offset, in.size() - offset);
            }
            return result_value;
        } catch (...) {
            retry_offset_ = offset;
            throw;
        }
    }

    // SLOW PATH: a buffered partial-frame tail is pending in input_. Own all new
    // bytes before dispatch. If dispatch throws, retain the cursor and do not
    // append the caller's retry span a second time. The consumed prefix is
    // reclaimed at the start of the next feed, so event views stay valid until then.
    if (!retry_offset) {
        input_.append(in.data(), in.size());
    }
    try {
        return dispatch_input(std::string_view(input_), input_offset_);
    } catch (...) {
        retry_offset_ = std::size_t{0};
        throw;
    }
}

void http2_connection::begin_connection() {
    if (preface_phase_ != preface_phase_type::not_started) {
        return;
    }
    std::array<char, http2_local_settings::frame_bytes + http2_window_update_frame_bytes> buffer;
    auto* out = http2_write_local_settings_frame(buffer.data(), enable_push_);
    if constexpr (http2_local_settings::initial_window_size >
                  static_cast<std::uint32_t>(http2_default_initial_window_size)) {
        out = http2_write_window_update(out, 0,
            http2_local_settings::initial_window_size -
                static_cast<std::uint32_t>(http2_default_initial_window_size));
    }
    const auto settings_bytes = static_cast<std::size_t>(out - buffer.data());
    const auto preface_bytes =
        role_ == http2_role::client ? http2_client_preface.size() : std::size_t{0};
    if (settings_bytes > std::numeric_limits<std::size_t>::max() - preface_bytes) {
        throw std::length_error("HTTP/2 connection preface output size overflow");
    }
    // Queue the complete local preface before publishing that this connection has
    // started. A failed reserve therefore leaves the exact begin_connection() call
    // retryable instead of exposing a half-started protocol state.
    output_.reserve_segments_additional(role_ == http2_role::client ? 2 : 1);
    output_.reserve_additional(preface_bytes + settings_bytes);
    if (role_ == http2_role::client) {
        output_.append_bytes(http2_client_preface);
    }
    output_.append_bytes(std::string_view(buffer.data(), settings_bytes));
    preface_phase_ = role_ == http2_role::client ? preface_phase_type::awaiting_peer_settings
                                                 : preface_phase_type::awaiting_client_magic;
}

}  // namespace ruvia::detail
