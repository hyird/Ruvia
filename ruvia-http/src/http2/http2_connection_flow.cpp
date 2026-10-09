#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "http2/http2_connection.h"
#include "http2/http2_flow_control.h"
#include "http2/http2_frame_codec.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_window_update.h"

// Connection- and stream-level flow control: how much of a queued body may go out
// under the current send window, what a WINDOW_UPDATE reopens, and the receive
// credit an owner's consumption returns to the peer.

namespace ruvia::detail {
namespace {

[[nodiscard]] std::size_t http2_data_frame_count(std::size_t data_bytes, std::size_t max_frame_size) {
    return data_bytes == 0 ? 0 : data_bytes / max_frame_size + (data_bytes % max_frame_size == 0 ? 0 : 1);
}

[[nodiscard]] std::size_t http2_data_frame_encoded_bytes(
    std::size_t data_bytes, std::size_t max_frame_size) {
    if (data_bytes == 0) {
        return 0;
    }
    const auto frame_count = data_bytes / max_frame_size + (data_bytes % max_frame_size == 0 ? 0 : 1);
    if (frame_count >
        (std::numeric_limits<std::size_t>::max() - data_bytes) / http2_frame_header_bytes) {
        throw std::length_error("HTTP/2 DATA output size overflow");
    }
    return data_bytes + frame_count * http2_frame_header_bytes;
}

// The largest HPACK integer encoding for a dynamic-table update is six bytes
// (the value is a uint32 with a five-bit prefix). The drain preflight uses this
// upper bound; append_response_header_frames computes the exact prefix afterwards.
[[nodiscard]] std::size_t http2_header_frame_encoded_bytes(
    std::size_t header_bytes, std::size_t max_frame_size, std::size_t first_prefix_bytes) {
    if (first_prefix_bytes > max_frame_size) {
        throw std::length_error("HTTP/2 header prefix exceeds frame size");
    }
    std::size_t encoded_bytes = 0;
    std::size_t offset = 0;
    bool first = true;
    while (offset < header_bytes || (first && first_prefix_bytes != 0)) {
        const auto prefix_bytes = first ? first_prefix_bytes : std::size_t{0};
        const auto chunk = std::min(header_bytes - offset, max_frame_size - prefix_bytes);
        const auto frame_bytes = http2_frame_header_bytes + prefix_bytes + chunk;
        if (frame_bytes > std::numeric_limits<std::size_t>::max() - encoded_bytes) {
            throw std::length_error("HTTP/2 header output size overflow");
        }
        encoded_bytes += frame_bytes;
        offset += chunk;
        first = false;
    }
    return encoded_bytes;
}

[[nodiscard]] std::size_t checked_output_bytes_add(std::size_t current, std::size_t additional) {
    if (additional > std::numeric_limits<std::size_t>::max() - current) {
        throw std::length_error("HTTP/2 deferred output size overflow");
    }
    return current + additional;
}

}  // namespace

std::size_t http2_connection::send_data_up_to_window(
    http2_stream_state& stream, std::string_view data, std::size_t offset, http2_end_stream end_stream) {
    const auto total = data.size();
    while (offset < total) {
        const auto available = http2_available_send_window(connection_send_window_, stream);
        if (available == 0) {
            break;  // window exhausted; caller buffers the remainder
        }
        const auto chunk =
            std::min<std::size_t>({total - offset, available, peer_settings_.max_frame_size()});
        const bool last = offset + chunk == total;
        output_.append_frame(http2_frame_type::data,
            static_cast<std::uint8_t>(http2_ends_stream(end_stream) && last ? http2_flag_end_stream : 0),
            stream.id(), data.substr(offset, chunk));
        http2_consume_send_window(connection_send_window_, stream, chunk);
        stream.commit_local_content(chunk);
        offset += chunk;
    }
    return offset;
}

void http2_connection::mark_send_window_opened() {
    // Drain is a wire/state transaction: a throwing PMR resource must not leave
    // a queued body with a smaller window and no corresponding bytes. Simulate
    // the complete drain first and reserve both outbound bytes and the
    // completion notification vector before touching any stream state.
    std::size_t required_output_bytes = 0;
    std::size_t required_output_segments = 0;
    std::size_t drained_count = 0;
    auto simulated_connection_window = connection_send_window_;
    bool simulated_table_update_pending = encoder_table_size_update_pending_;
    const auto max_frame = peer_settings_.max_frame_size();
    constexpr std::size_t max_dynamic_table_update_bytes = 6;
    for (const auto& pending : pending_sends_) {
        auto* stream = find_stream(pending.stream_id_);
        if (stream == nullptr || stream->is_aborted()) {
            continue;
        }
        if (pending.offset_ > pending.bytes_.size()) {
            throw std::logic_error("HTTP/2 deferred DATA offset is invalid");
        }
        const auto remaining = pending.bytes_.size() - pending.offset_;
        const auto available = http2_available_send_window(simulated_connection_window, *stream);
        const auto immediate = std::min(remaining, available);
        required_output_bytes = checked_output_bytes_add(
            required_output_bytes, http2_data_frame_encoded_bytes(immediate, max_frame));
        required_output_segments = checked_output_bytes_add(
            required_output_segments, http2_data_frame_count(immediate, max_frame));
        simulated_connection_window -= static_cast<std::int32_t>(immediate);
        if (immediate != remaining) {
            continue;
        }

        ++drained_count;
        if (!pending.trailer_block_.empty()) {
            const auto header_frame_bytes = http2_header_frame_encoded_bytes(pending.trailer_block_.size(),
                max_frame, simulated_table_update_pending ? max_dynamic_table_update_bytes : 0);
            required_output_bytes = checked_output_bytes_add(required_output_bytes, header_frame_bytes);
            required_output_segments = checked_output_bytes_add(required_output_segments,
                (header_frame_bytes + http2_frame_header_bytes - 1) /
                        (max_frame + http2_frame_header_bytes) +
                    1);
            simulated_table_update_pending = false;
        }
    }
    output_.reserve_additional(required_output_bytes);
    output_.reserve_segments_additional(required_output_segments);
    if (drained_count > drained_data_streams_.max_size() - drained_data_streams_.size()) {
        throw std::length_error("HTTP/2 drained stream notification size overflow");
    }
    drained_data_streams_.reserve(drained_data_streams_.size() + drained_count);

    // Drain core-owned DATA remainders now that a window may have opened. Completion
    // reports that the owner may submit the stream's next source chunk.
    for (std::size_t i = 0; i < pending_sends_.size();) {
        auto& pending = pending_sends_[i];
        auto* stream = find_stream(pending.stream_id_);
        if (stream == nullptr || stream->is_aborted()) {
            pending_sends_.erase(pending_sends_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        pending.offset_ = send_data_up_to_window(
            *stream, std::string_view(pending.bytes_), pending.offset_, pending.end_stream_);
        if (pending.offset_ >= pending.bytes_.size()) {
            // The body fully drained. If a trailer block was queued behind it, emit it
            // now as the terminal HEADERS(END_STREAM) -- strictly AFTER all the DATA.
            if (!pending.trailer_block_.empty() && !stream->is_aborted()) {
                append_response_header_frames(
                    *stream, std::string_view(pending.trailer_block_), http2_end_stream::end_stream);
            }
            if (http2_ends_stream(pending.end_stream_) || !pending.trailer_block_.empty()) {
                (void)stream->commit_local_end_stream();
                release_local_request_stream_if_closed(*stream);
            }
            const auto stream_id = pending.stream_id_;
            drained_data_streams_.push_back(stream_id);
            pending_sends_.erase(pending_sends_.begin() + static_cast<std::ptrdiff_t>(i));
            retire_completed_local_push(stream_id);
        } else {
            ++i;  // still window-blocked; keep the remainder for the next opening
        }
    }
}

bool http2_connection::process_window_update(
    const http2_frame_header& header_value, std::string_view payload_value) {
    if (payload_value.size() != 4) {
        append_goaway(http2_error_code::frame_size_error, "invalid WINDOW_UPDATE");
        return false;
    }
    const auto increment = http2_window_update_increment(payload_value);
    if (header_value.stream_id_ == 0) {
        switch (http2_apply_window_update(connection_send_window_, increment)) {
            case http2_window_update_result::ok:
                try {
                    mark_send_window_opened();
                } catch (...) {
                    connection_send_window_ -= static_cast<std::int32_t>(increment);
                    throw;
                }
                return true;
            case http2_window_update_result::zero_increment:
                append_goaway(http2_error_code::protocol_error, "zero connection WINDOW_UPDATE");
                return false;
            case http2_window_update_result::overflow:
                append_goaway(http2_error_code::flow_control_error, "connection window overflow");
                return false;
        }
        return true;
    }
    auto* stream = streams_.find(header_value.stream_id_);
    if (stream != nullptr && http2_stream_is_closed(*stream)) {
        // RFC 9113 section 6.9 permits a valid WINDOW_UPDATE on a closed
        // stream. A zero increment remains a stream PROTOCOL_ERROR, but no
        // RST_STREAM can legally be emitted after protocol closure.
        if (increment == 0) {
            append_goaway(http2_error_code::protocol_error, "zero WINDOW_UPDATE on closed stream");
            return false;
        }
        return true;
    }
    if (stream == nullptr) {
        if (is_idle_stream_id(header_value.stream_id_)) {
            append_goaway(http2_error_code::protocol_error, "WINDOW_UPDATE on idle stream");
            return false;
        }
        if (increment == 0) {
            // A skipped/released identifier is closed, not idle. Promote the
            // mandatory stream error because RST_STREAM is forbidden there.
            append_goaway(
                http2_error_code::protocol_error, "zero WINDOW_UPDATE on released closed stream");
            return false;
        }
        return true;
    }
    const auto reset_stream_for_window_update_error = [&](http2_error_code error) {
        const auto output_checkpoint = output_.checkpoint();
        try {
            output_.append_rst_stream(header_value.stream_id_, error);
            close_stream(header_value.stream_id_, http2_stream_close_source::local, error);
            mark_send_window_opened();
        } catch (...) {
            output_.rollback_to(output_checkpoint);
            throw;
        }
    };
    switch (http2_apply_stream_window_update(*stream, increment)) {
        case http2_window_update_result::ok:
            try {
                mark_send_window_opened();
            } catch (...) {
                (void)stream->add_send_window(-static_cast<std::int64_t>(increment));
                throw;
            }
            return true;
        case http2_window_update_result::zero_increment:
            reset_stream_for_window_update_error(http2_error_code::protocol_error);
            return true;
        case http2_window_update_result::overflow:
            reset_stream_for_window_update_error(http2_error_code::flow_control_error);
            return true;
    }
    return true;
}

void http2_connection::flush_window_debt(http2_stream_state& stream) {
    // Unreleased event credit must survive stream removal at CONNECTION scope, or an
    // owner that stops consuming after reset would permanently shrink the shared
    // window. It joins the same bounded batch as owner-consumed credit; a stream
    // WINDOW_UPDATE on a gone stream would instead be a peer protocol error.
    // Stream-scoped credit that was consumed but not yet advertised dies with
    // the stream. Its matching connection credit was queued independently.
    const auto stream_credit = stream.receive_window_credit().take();
    const auto debt = stream.take_window_debt();
    if (debt == 0) {
        return;
    }
    try {
        queue_consumed_data_credit(nullptr, debt);
    } catch (...) {
        stream.receive_window_credit().add(stream_credit);
        stream.add_window_debt(debt);
        throw;
    }
}

bool http2_connection::release_received_data(std::uint32_t stream_id, std::uint32_t bytes_value) {
    auto* stream = find_stream(stream_id);
    if (stream == nullptr) {
        return false;  // debt (if any) died with the stream; nothing left to credit
    }
    if (!stream->take_window_debt(bytes_value)) {
        return false;
    }
    try {
        queue_consumed_data_credit(stream, bytes_value);
    } catch (...) {
        stream->add_window_debt(bytes_value);
        throw;
    }
    return true;
}

void http2_connection::release_all_received_data(std::uint32_t stream_id) {
    auto* stream = find_stream(stream_id);
    if (stream == nullptr) {
        return;
    }
    const auto debt = stream->take_window_debt();
    if (debt == 0) {
        return;
    }
    try {
        queue_consumed_data_credit(stream, debt);
    } catch (...) {
        stream->add_window_debt(debt);
        throw;
    }
}

bool http2_connection::has_queued_data(std::uint32_t stream_id) const noexcept {
    return std::ranges::find(pending_sends_, stream_id, &http2_pending_send::stream_id_) != pending_sends_.end();
}

http2_data_queue_state http2_connection::data_queue_state(std::uint32_t stream_id) const noexcept {
    if (stream_aborted(stream_id)) {
        return http2_data_queue_state::aborted;
    }
    return has_queued_data(stream_id) ? http2_data_queue_state::queued
                                      : http2_data_queue_state::drained;
}

std::size_t http2_connection::pending_data_output_bytes(std::uint32_t stream_id) const noexcept {
    return output_.pending_data_bytes(stream_id);
}

std::optional<http2_send_window_state> http2_connection::send_window_state(
    std::uint32_t stream_id) const noexcept {
    const auto* stream = streams_.find(stream_id);
    if (stream == nullptr) {
        return std::nullopt;
    }
    const auto available = http2_available_send_window(connection_send_window_, *stream);
    return http2_send_window_state{
        .connection_window_ = connection_send_window_,
        .stream_window_ = stream->send_window(),
        .available_ = static_cast<std::uint32_t>(available),
        .queued_data_ = has_queued_data(stream_id),
    };
}

bool http2_connection::stream_aborted(std::uint32_t stream_id) const noexcept {
    const auto* stream = streams_.find(stream_id);
    return stream == nullptr || stream->is_aborted();
}

void http2_connection::queue_consumed_data_credit(http2_stream_state* stream, std::uint32_t bytes_value) {
    if (bytes_value == 0) {
        return;
    }

    const bool stream_can_receive =
        stream != nullptr && !http2_remote_peer_half_closed(*stream) && !stream->is_aborted();
    const auto connection_ready = connection_receive_credit_.ready_after(bytes_value);
    const auto stream_ready = stream_can_receive && stream->receive_window_credit().ready_after(bytes_value);
    const auto frame_count =
        static_cast<std::size_t>(connection_ready) + static_cast<std::size_t>(stream_ready);
    if (frame_count != 0) {
        output_.reserve_additional(frame_count * http2_window_update_frame_bytes);
    }

    connection_receive_credit_.add(bytes_value);
    if (stream_can_receive) {
        stream->receive_window_credit().add(bytes_value);
    }

    char buffer[http2_window_update_frame_bytes * 2];
    auto* out = buffer;
    if (connection_receive_credit_.ready()) {
        const auto increment = connection_receive_credit_.take();
        http2_credit_connection_receive_window(
            connection_receive_window_, static_cast<std::int32_t>(increment));
        out = http2_write_window_update(out, 0, increment);
    }
    if (stream_can_receive && stream->receive_window_credit().ready()) {
        const auto increment = stream->receive_window_credit().take();
        http2_credit_stream_receive_window(*stream, static_cast<std::int32_t>(increment));
        out = http2_write_window_update(out, stream->id(), increment);
    }
    if (out != buffer) {
        output_.append_bytes(std::string_view(buffer, static_cast<std::size_t>(out - buffer)));
    }
}

void http2_connection::release_dropped_data_connection_window(std::int32_t flow_bytes) {
    // Every structurally valid DATA frame reached this path only after the shared
    // connection debit succeeded. Return exactly that credit while keeping the
    // connection; no stream window survives an abandoned stream.
    if (flow_bytes <= 0) {
        return;
    }
    queue_consumed_data_credit(nullptr, static_cast<std::uint32_t>(flow_bytes));
}

}  // namespace ruvia::detail
