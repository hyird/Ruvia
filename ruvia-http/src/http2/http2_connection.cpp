#include "http2/http2_connection.h"

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

#include "http2/http2_frame_codec.h"
#include "http2/http2_frame_payload.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_window_update.h"

namespace ruvia::detail {

namespace {
// Defense-in-depth budgets for the sans-I/O h2 core. The core has no clock, so these are
// per-connection counters (not rates); both trip GOAWAY(ENHANCE_YOUR_CALM).
//
// PING flood: every non-ACK PING is echoed as an ACK into the outbound buffer. Cap the
// number of inbound PINGs seen since the owner last drained output; the counter resets on
// every output drain (consume_output/take_output = ACKs flushed), so healthy keepalive
// never trips and only a peer piling on PINGs faster than we can flush the ACKs does.
constexpr std::uint32_t http2_max_undrained_pings = 1000;
// SETTINGS flood (CVE-2019-9515): every non-ACK SETTINGS frame is likewise echoed as a
// SETTINGS ACK into the outbound buffer, so an unread peer piling on empty SETTINGS grows
// output unboundedly exactly as a PING flood would. Same undrained-count budget, reset on
// the same output drains; a peer legitimately re-tuning SETTINGS mid-connection sends only
// a handful and never trips.
constexpr std::uint32_t http2_max_undrained_settings = 1000;

[[nodiscard]] std::size_t http2_goaway_encoded_bytes(std::string_view debug) {
    constexpr std::size_t goaway_fixed_payload_bytes = 8;
    constexpr std::size_t fixed_bytes = http2_frame_header_bytes + goaway_fixed_payload_bytes;
    if (debug.size() > std::numeric_limits<std::size_t>::max() - fixed_bytes) {
        throw std::length_error("HTTP/2 GOAWAY output size overflow");
    }
    return fixed_bytes + debug.size();
}
}  // namespace

http2_connection::http2_connection(std::pmr::memory_resource* resource, http2_role role, bool enable_push, bool receive_origin_advertisements)
    : resource_(resource),
      input_(std::string_view{}, resource),
      output_(resource),
      streams_(resource),
      priorities_(resource),
      decoder_({.resource_ = resource}),
      peer_settings_(role),
      events_(std::make_move_iterator(static_cast<http2_event*>(nullptr)),
          std::make_move_iterator(static_cast<http2_event*>(nullptr)), resource),
      pending_sends_(std::size_t{0}, resource),
      drained_data_streams_(std::size_t{0}, resource),
      taken_drained_data_streams_(std::size_t{0}, resource),
      pinned_streams_(std::size_t{0}, resource),
      role_(role),
      connection_send_window_(http2_default_initial_window_size),
      connection_receive_window_(static_cast<std::int32_t>(http2_local_settings::initial_window_size)) {
    enable_push_ = role == http2_role::client && enable_push;
    receive_origin_advertisements_ = role == http2_role::client && receive_origin_advertisements;
    decoder_.set_max_dynamic_table_size(http2_local_settings::header_table_size);
}

// --- outbound byte buffer (batched writes) ------------------------------------

std::string_view http2_connection::pending_output() const& noexcept {
    return output_.pending();
}

http2_output_consume_status http2_connection::consume_output(std::size_t bytes_value) noexcept {
    const auto status = output_.consume(bytes_value);
    if (status == http2_output_consume_status::drained) {
        consecutive_pings_ = 0;     // outbound (incl. PING ACKs) fully flushed
        consecutive_settings_ = 0;  // outbound (incl. SETTINGS ACKs) fully flushed
    }
    return status;
}

http2_output_batch_result http2_connection::take_output_batch(std::size_t max_bytes,
    std::pmr::string& into, http2_data_output_observer_type observer, void* observer_context) {
    const auto result_value = output_.take_batch(max_bytes, into, observer, observer_context);
    if (result_value.status_ == http2_output_batch_status::taken && !output_.wants_write()) {
        consecutive_pings_ = 0;
        consecutive_settings_ = 0;
    }
    return result_value;
}

void http2_connection::take_output(std::pmr::string& into) {
    consecutive_pings_ = 0;     // outbound (incl. PING ACKs) is being flushed
    consecutive_settings_ = 0;  // outbound (incl. SETTINGS ACKs) is being flushed
    output_.take(into);
}

// --- event queue --------------------------------------------------------------

std::optional<http2_event> http2_connection::next_event() {
    auto* event = peek_event();
    if (event == nullptr) {
        return std::nullopt;
    }
    auto result_value = std::optional<http2_event>(std::move(*event));
    consume_event();
    return result_value;
}

http2_event* http2_connection::peek_event() & noexcept {
    if (event_offset_ < events_.size()) {
        return &events_[event_offset_];
    }
    events_.clear();
    event_offset_ = 0;
    return nullptr;
}

bool http2_connection::has_pending_events(std::uint32_t stream_id) const noexcept {
    for (std::size_t index = event_offset_; index < events_.size(); ++index) {
        if (events_[index].references_stream(stream_id)) {
            return true;
        }
    }
    return false;
}

void http2_connection::consume_event() noexcept {
    if (event_offset_ >= events_.size()) {
        std::terminate();
    }
    ++event_offset_;
}

void http2_connection::reserve_event_slots(std::size_t count) {
    if (count > events_.max_size() - events_.size()) {
        throw std::length_error("HTTP/2 event queue size overflow");
    }
    events_.reserve(events_.size() + count);
}

std::span<const std::uint32_t> http2_connection::take_drained_data_streams() & noexcept {
    // Swap-and-clear so each drain is reported exactly once; the returned span stays
    // valid until the next call (double buffer, no allocation churn).
    taken_drained_data_streams_.swap(drained_data_streams_);
    drained_data_streams_.clear();
    return taken_drained_data_streams_;
}

// =============================================================================
// Frame processing below is a 1:1 port of the retired coroutine session's pure
// logic: inline async_write became append-to-output_, coroutine resume became
// events / drained-data marking. The coroutine stack itself is deleted; this
// core is the single h2 implementation for both server and client roles.
// =============================================================================

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
    // Graceful drain (RFC 9113 §6.8): advertise GOAWAY(NO_ERROR) at the last accepted
    // stream id WITHOUT a connection error -- established streams keep running, and HEADERS
    // for a stream above the advertised id are refused in process_headers.
    if (local_connection_state_.open() == nullptr) {
        return;
    }
    // Keep the lifecycle transition and its GOAWAY as one externally visible
    // operation. The reservation is made while the state is still open, so a
    // failed allocation leaves begin_drain() retryable.
    output_.reserve_segments_additional(1);
    output_.reserve_additional(http2_goaway_encoded_bytes("connection draining"));
    if (!local_connection_state_.begin_graceful_drain(last_stream_id_)) {
        return;
    }
    output_.append_goaway_frame(last_stream_id_, http2_error_code::no_error, "connection draining");
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
    for (std::size_t offset = 0; offset < payload_value.size(); offset += 6) {
        const auto entry_value = http2_read_setting_entry(payload_value, offset);
        const auto result_value = candidate_value.apply(entry_value.id_, entry_value.value_);
        if (const auto* failure = result_value.failure()) {
            append_goaway(http2_peer_setting_error_code(failure->error()),
                http2_peer_setting_error_message(failure->error()));
            return false;
        }
        if (entry_value.id_ == http2_setting_id::header_table_size &&
            entry_value.value_ < encoder_dynamic_table_size_) {
            encoder_table_size_reduction = true;
        }
    }

    const auto initial_window_delta = static_cast<std::int64_t>(candidate_value.initial_window_size()) -
                                      static_cast<std::int64_t>(peer_settings_.initial_window_size());
    // http2_stream_table preflights every stream before changing any of them. Do
    // this while the live peer settings are still untouched; a rejected delta can
    // therefore emit GOAWAY without publishing a partially applied SETTINGS.
    if (!http2_apply_stream_send_window_delta(streams_, initial_window_delta)) {
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
    peer_settings_.replace_values_from(candidate_value);
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
    // Check the flood budget before applying this frame. The limit is based on
    // ACKs that are still queued, so the frame that would exceed it must not
    // consume any SETTINGS state before its fatal GOAWAY is constructed.
    if (consecutive_settings_ >= http2_max_undrained_settings) {
        append_goaway(http2_error_code::enhance_your_calm, "excessive SETTINGS");
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
    const auto previous_consecutive_settings = consecutive_settings_;
    const auto previous_initial_window_size = peer_settings_.initial_window_size();
    bool settings_applied = false;
    try {
        output_.reserve_additional(http2_frame_header_bytes);
        if (!apply_settings_payload(payload_value)) {
            return false;
        }
        settings_applied = true;
        if (preface_phase_ == preface_phase_type::awaiting_peer_settings) {
            preface_phase_ = preface_phase_type::ready;
        }
        // SETTINGS flood budget (CVE-2019-9515): bound non-ACK SETTINGS seen since output
        // was last drained, exactly like the PING flood, since each appends an ACK below.
        ++consecutive_settings_;
        output_.append_frame(http2_frame_type::settings, http2_flag_ack, 0, {});
        // SETTINGS_INITIAL_WINDOW_SIZE may have opened send windows: drain deferred DATA
        // and report streams whose core-owned remainder completed.
        mark_send_window_opened();
        return true;
    } catch (...) {
        if (settings_applied) {
            const auto applied_initial_window_delta =
                static_cast<std::int64_t>(peer_settings_.initial_window_size()) -
                static_cast<std::int64_t>(previous_initial_window_size);
            if (!http2_apply_stream_send_window_delta(streams_, -applied_initial_window_delta)) {
                std::terminate();
            }
            peer_settings_.replace_values_from(previous_settings);
            encoder_dynamic_table_size_ = previous_encoder_dynamic_table_size;
            encoder_table_size_update_pending_ = previous_encoder_table_size_update_pending;
            preface_phase_ = previous_preface_phase;
            consecutive_settings_ = previous_consecutive_settings;
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
        auto* const stream = find_stream(header_value.stream_id_);
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
        const auto output_checkpoint = output_.checkpoint();
        try {
            output_.append_rst_stream(header_value.stream_id_, http2_error_code::frame_size_error);
            if (stream != nullptr) {
                close_stream(header_value.stream_id_, http2_stream_close_source::local,
                    http2_error_code::frame_size_error);
            }
        } catch (...) {
            output_.rollback_to(output_checkpoint);
            throw;
        }
        return true;
    }
    // RFC 9113 deprecates the RFC 7540 priority tree. Retain frame-shape validation,
    // then ignore the advisory dependency and weight on streams in every state.
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

    const http2_peer_goaway goaway(
        http2_read31(reinterpret_cast<const unsigned char*>(payload_value.data())),
        static_cast<http2_error_code>(
            http2_read32(reinterpret_cast<const unsigned char*>(payload_value.data() + 4))));
    if (peer_goaway_ && goaway.last_stream_id() > peer_goaway_->last_stream_id()) {
        // RFC 9113 §6.8: increasing this value can make an already retried request
        // ambiguous, so the peer is not allowed to widen it on a later GOAWAY.
        append_goaway(http2_error_code::protocol_error, "GOAWAY last stream id increased");
        return false;
    }

    std::array<std::uint32_t, http2_local_settings::max_concurrent_streams> unprocessed_stream_ids{};
    std::size_t unprocessed_count = 0;
    if (role_ == http2_role::client) {
        bool response_started_above_last = false;
        streams_.for_each([&](http2_stream_state& stream) {
            if ((stream.id() & 1U) == 0 || stream.id() <= goaway.last_stream_id() || stream.is_aborted()) {
                return;
            }
            // A response head proves that the peer acted on this request. Claiming
            // otherwise would make replay unsafe, so reject the contradictory GOAWAY.
            if (http2_remote_final_head_decoded(stream) || stream.interim_response_count() != 0) {
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
            if ((stream.id() & 1U) == 0 && stream.id() > goaway.last_stream_id() && !stream.is_aborted()) {
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
    // publishing the peer event. begin_drain() reserves its complete output first,
    // so a throwing resource leaves both lifecycle sides retryable.
    begin_drain();
    peer_goaway_ = goaway;
    events_.push_back(http2_event::goaway(goaway));

    if (role_ == http2_role::server) {
        for (std::size_t i = 0; i < unprocessed_count; ++i) {
            (void)close_stream_impl(unprocessed_stream_ids[i], http2_stream_close_source::peer_goaway,
                goaway.error(), close_notification_type::emit_event);
        }
        return true;
    }

    // Higher locally initiated streams were never processed and can be replayed on a
    // new connection. Close them inside the protocol core so flow-control debt,
    // deferred DATA, stream-table storage, and peer concurrency slots cannot leak.
    for (std::size_t i = 0; i < unprocessed_count; ++i) {
        const auto stream_id = unprocessed_stream_ids[i];
        if (!close_stream_impl(stream_id, http2_stream_close_source::peer_goaway, goaway.error(),
                close_notification_type::owner_already_knows)) {
            continue;
        }
        events_.push_back(http2_event::request_unprocessed(stream_id));
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
        return true;  // our own ping's ack; ignore
    }
    // PING-flood budget: bound inbound PINGs seen since the owner last flushed output
    // (see http2_max_undrained_pings). A peer echoing keepalive normally lets us drain the
    // ACKs and resets the counter; one flooding PINGs without reading the ACKs trips.
    if (consecutive_pings_ >= http2_max_undrained_pings) {
        append_goaway(http2_error_code::enhance_your_calm, "excessive PING frames");
        return false;
    }
    // The flood counter describes ACKs that are now queued. Reserve before either
    // side effect so an allocation failure does not consume the peer's retryable
    // PING or make the counter lie about the output buffer.
    output_.reserve_additional(http2_frame_header_bytes + payload_value.size());
    output_.append_frame(http2_frame_type::ping, http2_flag_ack, 0, payload_value);  // echo back
    ++consecutive_pings_;
    return true;
}

http2_submit_status http2_connection::submit_priority_update(std::uint32_t stream_id, http_priority_fields fields_value) {
    if (role_ != http2_role::client || local_connection_state_.open() == nullptr || !stream_id ||
        stream_id > 0x7fffffffU || ((stream_id & 1) == 0 && is_idle_stream_id(stream_id))) {
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
    const auto id = http2_read32(reinterpret_cast<const unsigned char*>(payload_value.data())) & 0x7fffffffU;
    if (!id || ((id & 1) == 0 && is_idle_stream_id(id))) {
        append_goaway(http2_error_code::protocol_error, "invalid prioritized stream");
        return false;
    }
    std::erase_if(priorities_, [&](const auto& entry_value) {
        const auto* stream = find_stream(entry_value.first);
        return !is_idle_stream_id(entry_value.first) && (!stream || http2_stream_is_closed(*stream));
    });
    const auto update = decode_http2_priority_update(std::span(payload_value.data(), payload_value.size()));
    if ((update.index() != 0)) {
        return true;  // A malformed Priority field does not prioritize a stream.
    }
    std::size_t idle = 0;
    for (const auto& entry : priorities_) {
        if (is_idle_stream_id(entry.first)) {
            ++idle;
        }
    }
    const bool new_idle = is_idle_stream_id(id) && !priorities_.contains(id);
    std::size_t active = 0;
    streams_.for_each([&](const auto& stream) {
        if (!http2_stream_is_closed(stream) && stream.push_reservation() == http2_push_reservation::none) {
            ++active;
        }
    });
    if (active + idle + (new_idle ? 1u : 0u) > http2_local_settings::max_concurrent_streams) {
        append_goaway(http2_error_code::protocol_error, "too many prioritized idle streams");
        return false;
    }
    if (!is_idle_stream_id(id)) {
        const auto* stream = find_stream(id);
        if (!stream || http2_stream_is_closed(*stream) || stream->local_send().end_stream_committed() != nullptr) {
            return true;
        }
    }
    reserve_event_slots(1);
    priorities_.insert_or_assign(id, std::get<0>(update).fields_);
    events_.push_back(http2_event::priority_update(std::get<0>(update)));
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
    if (auto* stream = find_stream(header_value.stream_id_); stream && !stream->is_aborted() && header_value.type_ <= 9) {
        const auto reservation = stream->push_reservation();
        const auto type = static_cast<http2_frame_type>(header_value.type_);
        const bool permitted = type == http2_frame_type::rst_stream || type == http2_frame_type::priority ||
                               (reservation == http2_push_reservation::local && type == http2_frame_type::window_update) ||
                               (reservation == http2_push_reservation::remote &&
                                   (type == http2_frame_type::headers || type == http2_frame_type::continuation));
        if (reservation != http2_push_reservation::none && !permitted) {
            append_goaway(http2_error_code::protocol_error, "frame on reserved push stream");
            return false;
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
            if (header_value.stream_id_ != 0) {
                if (auto* stream = find_stream(header_value.stream_id_);
                    stream != nullptr && !http2_stream_is_closed(*stream) &&
                    stream->tunnel().open() != nullptr) {
                    // RFC 9113 8.5 narrows connected streams to DATA and the three
                    // stream-management frame types, even though unknown frames are
                    // normally ignored elsewhere.
                    const auto output_checkpoint = output_.checkpoint();
                    try {
                        output_.append_rst_stream(header_value.stream_id_, http2_error_code::protocol_error);
                        close_stream(header_value.stream_id_, http2_stream_close_source::local,
                            http2_error_code::protocol_error);
                    } catch (...) {
                        output_.rollback_to(output_checkpoint);
                        throw;
                    }
                }
            }
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
        if (header_value.length_ > http2_max_frame_size_limit || header_value.length_ > local_max_frame_size_) {
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

http2_feed_result http2_connection::feed(std::string_view in) {
    // Starting the role-specific preface is an explicit ownership boundary. In
    // particular, do not clear prior events, reclaim input, or copy caller bytes when
    // the driver forgot begin_connection(); the exact same span remains retryable.
    if (preface_phase_ == preface_phase_type::not_started) {
        return http2_feed_result::connection_not_started;
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
    const bool retrying_input = retry_input_;
    if (!retrying_input && input_offset_ > 0) {
        input_.erase(0, input_offset_);
        input_offset_ = 0;
    }
    events_.clear();
    event_offset_ = 0;

    // FAST PATH: nothing buffered and no preface pending -> parse exactly one complete
    // frame DIRECTLY over the caller's `in`. A span containing a second frame (or even
    // a partial tail) takes the owned slow path below. This is the boundary that keeps
    // a throwing later frame from making the caller retry an already committed prefix.
    if (input_.empty() && preface_phase_ != preface_phase_type::awaiting_client_magic &&
        in.size() >= http2_frame_header_bytes) {
        const auto header_value = http2_parse_frame_header(in.substr(0, http2_frame_header_bytes));
        if (in.size() - http2_frame_header_bytes == header_value.length_) {
            std::size_t offset = 0;
            if (!consume_frames(in, offset)) {
                return http2_feed_result::protocol_failure;
            }
            if (local_connection_state_.fatal_failure() != nullptr) {
                return http2_feed_result::protocol_failure;
            }
            return http2_feed_result::accepted;
        }
    }

    // SLOW PATH: a buffered partial-frame tail, a connection preface, or more than
    // one frame is pending. Own all new bytes before dispatch. If dispatch throws,
    // retain the cursor and do not append the caller's retry span a second time.
    if (!retrying_input) {
        input_.append(in.data(), in.size());
    }

    try {
        // Server mode: the 24-byte client connection preface precedes the first frame
        // (RFC 9113 §3.4). Consume + validate it before any frame parsing.
        if (preface_phase_ == preface_phase_type::awaiting_client_magic) {
            if (input_.size() - input_offset_ < http2_client_preface.size()) {
                return http2_feed_result::need_input;  // wait for the full preface
            }
            if (std::string_view(input_.data() + input_offset_, http2_client_preface.size()) !=
                http2_client_preface) {
                append_goaway(http2_error_code::protocol_error, "invalid connection preface");
                return http2_feed_result::protocol_failure;
            }
            input_offset_ += http2_client_preface.size();
            preface_phase_ = preface_phase_type::awaiting_peer_settings;
        }

        if (!consume_frames(std::string_view(input_), input_offset_)) {
            return http2_feed_result::protocol_failure;
        }
        // NOTE: the consumed prefix is reclaimed at the START of the next feed (see
        // above), so body-chunk views handed out via events stay valid until then.
        if (local_connection_state_.fatal_failure() != nullptr) {
            return http2_feed_result::protocol_failure;
        }
        retry_input_ = false;
        return input_offset_ < input_.size() ? http2_feed_result::need_input
                                             : http2_feed_result::accepted;
    } catch (...) {
        if (!retrying_input) {
            retry_input_ = true;
        }
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
