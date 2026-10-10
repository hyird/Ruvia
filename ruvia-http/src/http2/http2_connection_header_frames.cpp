#include <cstdint>
#include <optional>
#include <string_view>

#include "http2/http2_connection.h"
#include "http2/http2_frame_payload.h"
#include "http2/http2_header_block.h"
#include "http2/http2_header_rules.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_request_headers.h"

// HEADERS and CONTINUATION as they arrive: padding and priority framing, whether
// this stream may open at all, and keeping a header block contiguous until its
// END_HEADERS.

namespace ruvia::detail {

template <http2_header_block_kind kind>
bool http2_connection::complete_decoded_header_block(http2_stream_state& stream) {
    static_assert(kind == http2_header_block_kind::initial || kind == http2_header_block_kind::trailers);
    http2_stream_header_decode_transaction transaction{
        stream, kind == http2_header_block_kind::initial && role_ == http2_role::server};
    auto hpack_transaction = decoder_.begin_transaction();
    header_decode_status status = header_decode_status::protocol_error;
    if constexpr (kind == http2_header_block_kind::initial) {
        status = decode_initial_header_block(stream, transaction, hpack_transaction);
    } else {
        status = finish_trailer_block(stream, transaction, hpack_transaction);
    }
    if (status != header_decode_status::ok) {
        transaction.rollback();
        return handle_header_decode_failure(stream, status, &hpack_transaction);
    }
    transaction.commit();
    hpack_transaction.commit();
    if constexpr (kind == http2_header_block_kind::initial) {
        stream.activate_push();
    }
    http2_reset_header_block(stream);
    return true;
}

bool http2_connection::process_headers(const http2_frame_header& header_value, std::string_view payload_value) {
    if (header_value.stream_id_ == 0) {
        append_goaway(http2_error_code::protocol_error, "HEADERS stream id must be nonzero");
        return false;
    }
    if ((header_value.stream_id_ & 1U) == 0 && (role_ == http2_role::server || header_value.stream_id_ > last_peer_push_stream_id_)) {
        append_goaway(http2_error_code::protocol_error,
            role_ == http2_role::client ? "HEADERS on even stream id" : "invalid client stream id");
        return false;
    }

    std::string_view fragment;
    switch (http2_decode_headers_payload(header_value, payload_value, fragment)) {
        case http2_frame_payload_status::decoded:
            break;
        case http2_frame_payload_status::invalid_padding:
            append_goaway(http2_error_code::protocol_error, "invalid HEADERS padding");
            return false;
        case http2_frame_payload_status::missing_priority_fields:
            // HEADERS carries a field block and can alter HPACK state, so a payload
            // too short for its mandatory fields is a connection FRAME_SIZE_ERROR
            // (RFC 9113 §4.2), unlike the stream-scoped standalone PRIORITY frame.
            append_goaway(http2_error_code::frame_size_error, "HEADERS priority fields are incomplete");
            return false;
    }

    http2_stream_state* stream = nullptr;
    bool new_peer_stream = false;
    bool created_peer_stream = false;
    const auto previous_last_stream_id = last_stream_id_;
    std::optional<discarded_header_action_type> discarded_action;
    if (auto* existing = find_stream(header_value.stream_id_); existing != nullptr) {
        if (existing->is_aborted()) {
            if (existing->local_send().aborted()->source() == http2_stream_close_source::peer) {
                // Frames sent after the peer's own RST are not an in-flight race: the
                // connection byte stream orders them after that terminal signal.
                append_goaway(http2_error_code::stream_closed, "HEADERS after peer RST_STREAM");
                return false;
            }
            // After WE sent RST_STREAM, any peer frames already in flight must be
            // minimally processed and discarded. Do not send a second RST.
            discarded_action = discarded_header_action_type::ignore;
        } else if (http2_stream_is_closed(*existing)) {
            // A pin can retain normally completed request storage after both
            // protocol halves have closed. Decode for HPACK synchronization,
            // but never make storage retention authorize another stream frame.
            discarded_action = discarded_header_action_type::ignore;
        } else if (http2_remote_final_head_decoded(*existing) &&
                   (existing->tunnel().open() != nullptr ||
                       (role_ == http2_role::server && existing->tunnel().pending() != nullptr))) {
            // CONNECT has no request trailers, and an accepted connected stream only
            // permits DATA/RST_STREAM/WINDOW_UPDATE/PRIORITY. Decode the complete
            // field block for HPACK synchronization, then reset this stream.
            return start_discarded_header_block(
                header_value, fragment, discarded_header_action_type::reset_protocol_error);
        } else if (http2_remote_final_head_decoded(*existing)) {
            return process_trailer_headers(*existing, header_value, fragment);
        } else if (role_ == http2_role::client) {
            // A 1xx interim head was decoded on this stream; this block is the next
            // (possibly final) response head -- decode it through the shared tail.
            stream = existing;
        } else {
            // A second initial request head is a stream error. Decode its complete
            // field block before applying the reset so HPACK remains synchronized.
            discarded_action = discarded_header_action_type::reset_protocol_error;
        }
    } else if (role_ == http2_role::client) {
        if (is_idle_stream_id(header_value.stream_id_)) {
            append_goaway(http2_error_code::protocol_error, "HEADERS on idle stream");
            return false;
        }
        if (closed_streams_.source(header_value.stream_id_) == http2_stream_close_source::peer) {
            append_goaway(http2_error_code::stream_closed, "HEADERS after peer RST_STREAM");
            return false;
        }
        // A locally cancelled/completed or GOAWAY-rejected client stream no longer has
        // storage. Decode any late block into scratch so HPACK stays synchronized.
        discarded_action = discarded_header_action_type::ignore;
    } else {
        if (header_value.stream_id_ <= last_stream_id_) {
            const auto source_value = closed_streams_.source(header_value.stream_id_);
            if (source_value == http2_stream_close_source::peer) {
                append_goaway(http2_error_code::stream_closed, "HEADERS after peer RST_STREAM");
                return false;
            }
            if (!source_value.has_value()) {
                // HEADERS is the only frame that could establish this peer stream, but
                // a newly established identifier must be greater than every identifier
                // the peer already opened (RFC 9113 5.1.1). A skipped lower identifier
                // cannot be reopened as a new request.
                append_goaway(
                    http2_error_code::protocol_error, "new peer stream id is not increasing");
                return false;
            }
            // A stream explicitly closed by this endpoint can still have an in-flight
            // field block. Minimally decode and discard it so HPACK remains synchronized
            // as permitted by RFC 9113 5.1.
            discarded_action = discarded_header_action_type::ignore;
        } else {
            // Publish a genuinely new peer stream ID only after the first HEADERS
            // fragment has been accepted. A throwing stream allocation or header
            // buffer append must leave the complete frame retryable; advancing the
            // high-water mark before that point would turn the retry into a false
            // "stream id is not increasing" connection error.
            std::size_t prioritized_idle = 0, active = 0;
            for (const auto& [id, priority] : priorities_) {
                if (id > header_value.stream_id_ && is_idle_stream_id(id)) {
                    ++prioritized_idle;
                }
            }
            streams_.for_each([&](const auto& item) {
                // The advertised limit bounds streams the peer initiated (RFC 9113
                // 5.1.2); active server pushes count against the client's limit.
                if ((item.id() & 1U) != 0 && !http2_stream_is_closed(item) &&
                    item.push_reservation() == http2_push_reservation::none) {
                    ++active;
                }
            });
            // A HEADERS frame that itself exceeds the advertised limit is a stream
            // error (RFC 9113 5.1.2): refuse it below. Only idle-stream priorities
            // that crowd out an otherwise admissible stream end the connection.
            const bool concurrency_exceeded = active >= http2_local_settings::max_concurrent_streams;
            if (!concurrency_exceeded &&
                prioritized_idle + active + 1 > http2_local_settings::max_concurrent_streams) {
                append_goaway(http2_error_code::protocol_error, "idle priorities and active streams exceed concurrency");
                return false;
            }
            new_peer_stream = true;
            const auto* graceful_drain = local_connection_state_.graceful_drain();
            const bool drain_refused =
                graceful_drain != nullptr && header_value.stream_id_ > graceful_drain->last_stream_id();
            stream = drain_refused || concurrency_exceeded ? nullptr : create_stream(header_value.stream_id_);
            created_peer_stream = stream != nullptr;
            if (stream == nullptr) {
                discarded_action = discarded_header_action_type::refuse_stream;
            }
        }
    }

    if (discarded_action) {
        const auto result_value = start_discarded_header_block(header_value, fragment, *discarded_action);
        if (new_peer_stream && result_value) {
            last_stream_id_ = header_value.stream_id_;
        }
        return result_value;
    }

    // Buffering the compressed fragment is the first fallible operation. Do it
    // before publishing END_STREAM in the remote lifecycle; if the PMR rejects the
    // append, a retry sees the original head-pending state. A newly created stream
    // is likewise removed on this failure because it has not become observable yet.
    bool started_header_block = false;
    try {
        started_header_block = http2_start_header_block(*stream, fragment);
    } catch (...) {
        if (created_peer_stream) {
            streams_.remove(header_value.stream_id_);
        }
        throw;
    }
    if (!started_header_block) {
        if (created_peer_stream) {
            streams_.remove(header_value.stream_id_);
        }
        // The block exceeds the header buffer cap. We cannot decode a block we could
        // not fully buffer, and skipping it would desync the connection-global HPACK
        // dynamic table for every later block (RFC 9113 §4.3) -- so this is a
        // COMPRESSION_ERROR, not a survivable stream reset.
        append_goaway(http2_error_code::compression_error, "field block not decompressed");
        return false;
    }

    if (new_peer_stream) {
        last_stream_id_ = header_value.stream_id_;
    }

    bool recorded_head_end_stream = false;
    if ((header_value.flags_ & http2_flag_end_stream) != 0) {
        if (!stream->record_remote_head_end_stream()) {
            output_.append_rst_stream(header_value.stream_id_, http2_error_code::protocol_error);
            close_stream(
                header_value.stream_id_, http2_stream_close_source::local, http2_error_code::protocol_error);
            return true;
        }
        recorded_head_end_stream = true;
    }

    if ((header_value.flags_ & http2_flag_end_headers) != 0) {
        const auto output_checkpoint = output_.checkpoint();
        try {
            return complete_decoded_header_block<http2_header_block_kind::initial>(*stream);
        } catch (...) {
            output_.rollback_to(output_checkpoint);
            if (recorded_head_end_stream) {
                (void)stream->rollback_remote_head_end_stream_for_retry();
            }
            if (created_peer_stream) {
                streams_.remove(header_value.stream_id_);
            }
            if (new_peer_stream) {
                last_stream_id_ = previous_last_stream_id;
            }
            throw;
        }
    } else {
        header_continuation_.start(stream->id(), http2_header_block_kind::initial);
    }
    return true;
}

bool http2_connection::process_trailer_headers(
    http2_stream_state& stream, const http2_frame_header& header_value, std::string_view fragment) {
    if (http2_remote_peer_half_closed(stream)) {
        return start_discarded_header_block(
            header_value, fragment, discarded_header_action_type::reset_stream_closed);
    }
    if (stream.remote_receive().content_open() == nullptr) {
        return start_discarded_header_block(
            header_value, fragment, discarded_header_action_type::reset_protocol_error);
    }
    if ((header_value.flags_ & http2_flag_end_stream) == 0) {
        return start_discarded_header_block(
            header_value, fragment, discarded_header_action_type::reset_protocol_error);
    }
    if (!http2_start_header_block(stream, fragment)) {
        // Un-bufferable trailer block: same HPACK-consistency reasoning as HEADERS --
        // COMPRESSION_ERROR rather than a survivable stream reset.
        append_goaway(http2_error_code::compression_error, "field block not decompressed");
        return false;
    }

    if ((header_value.flags_ & http2_flag_end_headers) != 0) {
        const auto output_checkpoint = output_.checkpoint();
        try {
            return complete_decoded_header_block<http2_header_block_kind::trailers>(stream);
        } catch (...) {
            output_.rollback_to(output_checkpoint);
            throw;
        }
    } else {
        header_continuation_.start(stream.id(), http2_header_block_kind::trailers);
    }
    return true;
}

bool http2_connection::process_continuation(
    const http2_frame_header& header_value, std::string_view payload_value) {
    if (!header_continuation_.matches(header_value.stream_id_)) {
        append_goaway(http2_error_code::protocol_error, "invalid CONTINUATION");
        return false;
    }
    // Bound the CONTINUATION count per header block. Empty CONTINUATION frames add
    // no bytes and so never trip the accumulated-block size cap; without this an
    // endless stream of them keeps the block "in progress" forever (RFC 9113 §6.10,
    // CVE-2024-27316 CONTINUATION flood). Check without mutating first: buffering
    // the fragment below can allocate, and a failed retry must not spend the
    // frame-count budget twice.
    if (!header_continuation_.continuation_frame_budget_available()) {
        append_goaway(http2_error_code::enhance_your_calm, "CONTINUATION flood");
        return false;
    }
    const auto kind = header_continuation_.kind();
    if (kind == http2_header_block_kind::push_promise) {
        return process_push_continuation(header_value, payload_value);
    }
    http2_stream_state* stream = nullptr;
    if (kind == http2_header_block_kind::discarded) {
        if (!discarded_header_stream_ || discarded_header_stream_->id() != header_value.stream_id_) {
            append_goaway(http2_error_code::protocol_error, "missing discarded CONTINUATION state");
            return false;
        }
        // Prefer detached storage even when a pinned reset stream with the same ID is
        // still present. Its request views are owner-held and must remain immutable.
        stream = &*discarded_header_stream_;
    } else {
        stream = find_stream(header_value.stream_id_);
        if (stream == nullptr || stream->is_aborted()) {
            append_goaway(http2_error_code::protocol_error, "missing live CONTINUATION stream");
            return false;
        }
    }
    const auto continuation_checkpoint = header_continuation_.checkpoint();
    const auto header_block_checkpoint = stream->remote_header_block().size();
    const auto output_checkpoint = output_.checkpoint();
    try {
        if (!http2_append_header_block(*stream, payload_value)) {
            // The accumulated HEADERS+CONTINUATION block overflowed the buffer cap; the
            // partial block cannot be decoded, so skipping it would desync HPACK for the
            // whole connection -- COMPRESSION_ERROR (RFC 9113 §4.3), not a stream reset.
            append_goaway(http2_error_code::compression_error, "field block not decompressed");
            return false;
        }
        (void)header_continuation_.record_continuation_frame();
        if ((header_value.flags_ & http2_flag_end_headers) != 0) {
            const auto completed_kind = header_continuation_.finish_kind();
            if (completed_kind == http2_header_block_kind::discarded) {
                return finish_discarded_header_block();
            }
            if (completed_kind == http2_header_block_kind::trailers) {
                return complete_decoded_header_block<http2_header_block_kind::trailers>(*stream);
            } else {
                return complete_decoded_header_block<http2_header_block_kind::initial>(*stream);
            }
        }
    } catch (...) {
        // Appending this CONTINUATION fragment is part of the same retryable frame
        // transaction as decoding and event/output publication. If a later
        // allocation fails, the caller will retry the exact same frame; restore the
        // compressed block prefix as well as the continuation latch so the fragment
        // is not decoded twice.
        output_.rollback_to(output_checkpoint);
        stream->remote_header_block().resize(header_block_checkpoint);
        header_continuation_.restore(continuation_checkpoint);
        throw;
    }
    return true;
}

}  // namespace ruvia::detail
