#include <algorithm>

#include "http2/http2_connection.h"
#include "http2/http2_flow_control.h"
#include "http2/http2_frame_codec.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_window_update.h"

// Connection- and stream-level flow control. WINDOW_UPDATE only restores send
// credit; the owner retries its unconsumed DATA after observing the new windows.

namespace ruvia::detail {

void http2_connection::append_data_frames(
    http2_stream_state& stream, std::string_view data, http2_end_stream end_stream) {
    const auto total = data.size();
    std::size_t offset = 0;
    while (offset < total) {
        const auto chunk = std::min<std::size_t>(total - offset, peer_settings_.max_frame_size());
        const bool last = offset + chunk == total;
        output_.append_frame(http2_frame_type::data,
            static_cast<std::uint8_t>(http2_ends_stream(end_stream) && last ? http2_flag_end_stream : 0),
            stream.id(), data.substr(offset, chunk));
        http2_consume_send_window(connection_send_window_, stream, chunk);
        stream.commit_local_content(chunk);
        offset += chunk;
    }
}

bool http2_connection::process_window_update(
    const http2_frame_header& header_value, std::string_view payload_value) {
    if (payload_value.size() != 4) {
        append_goaway(http2_error_code::frame_size_error, "invalid WINDOW_UPDATE");
        return false;
    }
    const auto increment = http2_window_update_increment(payload_value);
    if (receive_frame_metadata_) {
        // Reserve notification storage before changing either send window.
        reserve_event_slots(1);
    }
    if (header_value.stream_id_ == 0) {
        switch (http2_apply_window_update(connection_send_window_, increment)) {
            case http2_window_update_result::ok:
                if (receive_frame_metadata_) {
                    events_.push_back(http2_event(http2_received_window_update{
                        .stream_id_ = header_value.stream_id_, .increment_ = increment}));
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
    if (was_closed_by_peer_reset(header_value.stream_id_, stream)) {
        append_goaway(http2_error_code::stream_closed, "WINDOW_UPDATE after peer RST_STREAM");
        return false;
    }
    if (stream != nullptr && http2_stream_is_closed(*stream)) {
        // RFC 9113 section 5.1 (closed): WINDOW_UPDATE received in this state
        // MUST be ignored, whatever its increment; the section 6.9 zero-increment
        // stream error applies to a stream that still exists.
        return true;
    }
    if (stream == nullptr) {
        if (is_idle_stream_id(header_value.stream_id_)) {
            append_goaway(http2_error_code::protocol_error, "WINDOW_UPDATE on idle stream");
            return false;
        }
        // A skipped/released identifier is closed, not idle: ignored as above.
        return true;
    }
    switch (http2_apply_stream_window_update(*stream, increment)) {
        case http2_window_update_result::ok:
            if (receive_frame_metadata_) {
                events_.push_back(http2_event(http2_received_window_update{
                    .stream_id_ = header_value.stream_id_, .increment_ = increment}));
            }
            return true;
        case http2_window_update_result::zero_increment:
            reset_stream(header_value.stream_id_, http2_error_code::protocol_error);
            return true;
        case http2_window_update_result::overflow:
            reset_stream(header_value.stream_id_, http2_error_code::flow_control_error);
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

std::size_t http2_connection::pending_data_output_bytes(std::uint32_t stream_id) const noexcept {
    return output_.pending_data_bytes(stream_id);
}

std::optional<http2_send_window_state> http2_connection::send_window_state(
    std::uint32_t stream_id) const noexcept {
    const auto* stream = streams_.find(stream_id);
    if (stream == nullptr || http2_stream_is_closed(*stream)) {
        return std::nullopt;
    }
    const auto available = http2_available_send_window(connection_send_window_, *stream);
    return http2_send_window_state{
        .connection_window_ = connection_send_window_,
        .stream_window_ = stream->send_window(),
        .available_ = static_cast<std::uint32_t>(available),
    };
}

bool http2_connection::stream_aborted(std::uint32_t stream_id) const noexcept {
    const auto* stream = streams_.find(stream_id);
    return stream == nullptr || stream->is_aborted();
}

void http2_connection::reserve_consumed_data_credit(http2_stream_state* stream, std::uint32_t bytes_value) {
    if (local_connection_state_.fatal_failure() != nullptr || bytes_value == 0) {
        return;
    }

    const bool stream_can_receive =
        stream != nullptr && !http2_remote_peer_half_closed(*stream) && !stream->is_aborted();
    const auto connection_ready = connection_receive_credit_.ready_after(bytes_value);
    const auto stream_ready = stream_can_receive && stream->receive_window_credit().ready_after(bytes_value);
    const auto frame_count =
        static_cast<std::size_t>(connection_ready) + static_cast<std::size_t>(stream_ready);
    if (frame_count != 0) {
        // Both WINDOW_UPDATE frames share one raw output segment. Reserve its
        // metadata before changing the receive windows or their pending credit.
        output_.reserve_segments_additional(1);
        output_.reserve_additional(frame_count * http2_window_update_frame_bytes);
    }
}

void http2_connection::queue_consumed_data_credit(http2_stream_state* stream, std::uint32_t bytes_value) {
    if (local_connection_state_.fatal_failure() != nullptr) {
        // The caller still settles its exact stream debt, but a terminal
        // connection must not append WINDOW_UPDATE behind the fatal GOAWAY.
        return;
    }
    reserve_consumed_data_credit(stream, bytes_value);
    if (bytes_value == 0) {
        return;
    }

    const bool stream_can_receive =
        stream != nullptr && !http2_remote_peer_half_closed(*stream) && !stream->is_aborted();
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
