#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#include "http2/http2_connection.h"
#include "http2/http2_flow_control.h"
#include "http2/http2_frame_codec.h"
#include "http2/http2_frame_payload.h"
#include "http2/http2_remote_receive_semantics.h"

// Inbound DATA: framing and padding validation, the receive-window accounting a
// payload consumes, and delivering the bytes to the stream that owns them.

namespace ruvia::detail {

bool http2_connection::process_data(const http2_frame_header& header_value, std::string_view payload_value) {
    if (header_value.stream_id_ == 0) {
        append_goaway(http2_error_code::protocol_error, "DATA stream id must be nonzero");
        return false;
    }
    if ((header_value.stream_id_ & 1U) == 0 && (role_ == http2_role::server || header_value.stream_id_ > last_peer_push_stream_id_)) {
        append_goaway(http2_error_code::protocol_error, "DATA on invalid client stream id");
        return false;
    }
    std::string_view data;
    if (!http2_decode_data_payload(header_value, payload_value, data)) {
        // Invalid padding is a frame-structure error for the connection. Validate it
        // before any stream-phase shortcut (closed body, pending CONNECT, reset) can
        // incorrectly downgrade it to a stream error.
        append_goaway(http2_error_code::protocol_error, "invalid DATA padding");
        return false;
    }

    const auto flow_bytes = static_cast<std::int32_t>(payload_value.size());
    // Feed retries the complete frame when a PMR allocation throws. Do not debit the
    // connection window until the selected branch has reserved all of its output and
    // event capacity; otherwise a retry would observe a half-processed DATA frame.
    if (flow_bytes > connection_receive_window_) {
        append_goaway(http2_error_code::flow_control_error, "connection flow-control window exceeded");
        return false;
    }

    const bool end_stream = (header_value.flags_ & http2_flag_end_stream) != 0;
    const auto reserve_events = [this](std::size_t count) { reserve_event_slots(count); };
    const auto reserve_consumed_credit = [this](http2_stream_state* credit_stream, std::uint32_t bytes_value) {
        if (bytes_value == 0) {
            return;
        }
        const bool stream_can_receive = credit_stream != nullptr &&
                                        !http2_remote_peer_half_closed(*credit_stream) &&
                                        !credit_stream->is_aborted();
        const auto connection_ready = connection_receive_credit_.ready_after(bytes_value);
        const auto stream_ready =
            stream_can_receive && credit_stream->receive_window_credit().ready_after(bytes_value);
        const auto frame_count =
            static_cast<std::size_t>(connection_ready) + static_cast<std::size_t>(stream_ready);
        if (frame_count != 0) {
            output_.reserve_additional(frame_count * http2_window_update_frame_bytes);
        }
    };
    const auto reserve_stream_error = [this, &reserve_events](std::size_t extra_events = 0) {
        reserve_events(1 + extra_events);
        // RST_STREAM plus at most one WINDOW_UPDATE from close_stream's flushed
        // stream debt and one from this frame's dropped connection credit.
        output_.reserve_additional(http2_frame_header_bytes + 4 + 2 * http2_window_update_frame_bytes);
    };
    const auto debit_connection = [this, flow_bytes]() noexcept {
        connection_receive_window_ -= flow_bytes;
    };

    auto* stream = find_stream(header_value.stream_id_);
    if (was_closed_by_peer_reset(header_value.stream_id_, stream)) {
        // This peer's RST_STREAM and later DATA are ordered on the same connection.
        // Unlike DATA that was already in flight when we sent a reset, this cannot be
        // a state-view race; keep the strict closed-state verdict.
        append_goaway(http2_error_code::stream_closed, "DATA after peer RST_STREAM");
        return false;
    }
    if (stream == nullptr) {
        const auto close_source = closed_streams_.source(header_value.stream_id_);
        if (close_source == http2_stream_close_source::peer_goaway ||
            !is_idle_stream_id(header_value.stream_id_)) {
            // A dropped DATA frame still consumes connection flow control, but has no
            // stream window or application event to publish.
            reserve_consumed_credit(nullptr, static_cast<std::uint32_t>(flow_bytes));
            debit_connection();
            release_dropped_data_connection_window(flow_bytes);
            return true;
        }
        append_goaway(http2_error_code::protocol_error, "DATA before HEADERS");
        return false;
    }
    if (http2_stream_is_closed(*stream)) {
        reserve_consumed_credit(nullptr, static_cast<std::uint32_t>(flow_bytes));
        debit_connection();
        release_dropped_data_connection_window(flow_bytes);
        return true;
    }

    const auto reset_stream = [&](http2_error_code error, bool debit_stream_window) {
        reserve_stream_error();
        debit_connection();
        if (debit_stream_window) {
            (void)stream->consume_receive_window(flow_bytes);
        }
        output_.append_rst_stream(header_value.stream_id_, error);
        close_stream(header_value.stream_id_, http2_stream_close_source::local, error);
        release_dropped_data_connection_window(flow_bytes);
    };

    const auto& remote = stream->remote_receive();
    if (remote.head_pending() != nullptr || remote.head_end_stream_pending() != nullptr) {
        reset_stream(http2_error_code::protocol_error, false);
        return true;
    }
    if (remote.end_stream() != nullptr || remote.connect_pending_end_stream() != nullptr) {
        // END_STREAM closes only the peer's send half. Another DATA frame from this
        // peer is nevertheless a frame on a half-closed (remote) stream.
        reset_stream(http2_error_code::stream_closed, false);
        return true;
    }

    const bool pending_connect_control = remote.connect_pending() != nullptr;
    const bool rejected_connect_terminal = remote.connect_rejected_awaiting_end_stream() != nullptr;
    const bool tunnel_data = remote.tunnel_open() != nullptr;
    const bool content_data = remote.content_open() != nullptr;
    const bool metadata_only_content =
        content_data && (stream->remote_content().metadata_only_without_length() != nullptr ||
                            stream->remote_content().metadata_only_known_length() != nullptr);
    if (!pending_connect_control && !rejected_connect_terminal && !tunnel_data && !content_data) {
        reset_stream(http2_error_code::stream_closed, false);
        return true;
    }

    // Check the stream window without mutating it so a flow-control reset can be
    // fully reserved before either receive window is consumed.
    if (flow_bytes > stream->receive_window()) {
        reset_stream(http2_error_code::flow_control_error, false);
        return true;
    }

    if (pending_connect_control || rejected_connect_terminal) {
        // CONNECT has no request content. Empty DATA remains framing-only until its
        // END_STREAM, while padding still consumes both flow-control windows.
        if (!data.empty()) {
            reset_stream(http2_error_code::protocol_error, true);
            return true;
        }
        reserve_consumed_credit(end_stream ? nullptr : stream, static_cast<std::uint32_t>(flow_bytes));
        debit_connection();
        (void)stream->consume_receive_window(flow_bytes);
        if (flow_bytes > 0) {
            queue_consumed_data_credit(
                end_stream ? nullptr : stream, static_cast<std::uint32_t>(flow_bytes));
        }
        if (!end_stream) {
            return true;
        }
        const bool remote_finished = pending_connect_control ? stream->finish_remote_pending_connect()
                                                             : stream->finish_remote_rejected_connect();
        if (!remote_finished) {
            // The selected remote alternative is exclusive and the transition is
            // noexcept; reaching this branch means an internal invariant failed.
            std::terminate();
        }
        release_local_request_stream_if_closed(*stream);
        return true;
    }

    // Evaluate content accounting on a detached state first. A rejected DATA frame
    // must reserve its reset before the live content counter is changed.
    http2_remote_content_accounting_result content_accounting =
        http2_remote_content_accounting_result::accepted;
    http2_remote_content_state candidate_content = stream->remote_content();
    if (content_data) {
        content_accounting = candidate_content.account(data.size());
        if (content_accounting == http2_remote_content_accounting_result::counter_overflow) {
            reset_stream(http2_error_code::cancel, true);
            return true;
        }
        if (content_accounting != http2_remote_content_accounting_result::accepted) {
            reset_stream(http2_error_code::protocol_error, true);
            return true;
        }
        if (end_stream && !candidate_content.terminal_length_valid()) {
            // Preserve the already-received body chunk before reporting the
            // Content-Length mismatch. The body event and the close event are one
            // pre-reserved publication batch; the close path flushes the matching
            // receive debt, so this branch must not return the same bytes again via
            // release_dropped_data_connection_window().
            reserve_stream_error(data.empty() ? 0 : 1);
            if (data.empty() && flow_bytes > 0) {
                reserve_consumed_credit(nullptr, static_cast<std::uint32_t>(flow_bytes));
            }
            debit_connection();
            (void)stream->consume_receive_window(flow_bytes);
            (void)stream->account_remote_content(data.size());
            if (flow_bytes > 0) {
                if (!data.empty()) {
                    stream->add_window_debt(static_cast<std::uint32_t>(flow_bytes));
                    events_.push_back(http2_event::message_body_chunk(
                        header_value.stream_id_, data, static_cast<std::uint32_t>(flow_bytes)));
                } else {
                    queue_consumed_data_credit(nullptr, static_cast<std::uint32_t>(flow_bytes));
                }
            }
            output_.append_rst_stream(header_value.stream_id_, http2_error_code::protocol_error);
            close_stream(
                header_value.stream_id_, http2_stream_close_source::local, http2_error_code::protocol_error);
            return true;
        }
    }

    const bool deliver_data = !metadata_only_content && !data.empty();
    if (deliver_data) {
        reserve_events(end_stream ? 2 : 1);
    } else if (end_stream) {
        reserve_events(1);
    }
    if (!deliver_data && flow_bytes > 0) {
        reserve_consumed_credit(end_stream ? nullptr : stream, static_cast<std::uint32_t>(flow_bytes));
    }

    debit_connection();
    (void)stream->consume_receive_window(flow_bytes);
    if (content_data) {
        (void)stream->account_remote_content(data.size());
    }
    if (flow_bytes > 0) {
        if (deliver_data) {
            // Advertise new capacity only after the owner has consumed or copied the
            // borrowed event bytes; this preserves backpressure for content and tunnel
            // data alike.
            stream->add_window_debt(static_cast<std::uint32_t>(flow_bytes));
        } else {
            // Empty/padding-only DATA has no application event, so its credit is
            // returned immediately in the same bounded batch.
            queue_consumed_data_credit(
                end_stream ? nullptr : stream, static_cast<std::uint32_t>(flow_bytes));
        }
    }

    // Hand only actual body bytes to the owner. Empty and padding-only DATA still
    // participate in framing, END_STREAM, and flow control without creating a
    // no-progress event for every tiny wire frame.
    if (deliver_data) {
        events_.push_back(tunnel_data ? http2_event::tunnel_data(header_value.stream_id_, data,
                                            static_cast<std::uint32_t>(flow_bytes))
                                      : http2_event::message_body_chunk(header_value.stream_id_, data,
                                            static_cast<std::uint32_t>(flow_bytes)));
    }
    if (end_stream) {
        const bool remote_finished =
            tunnel_data ? stream->finish_remote_tunnel() : stream->finish_remote_content();
        if (!remote_finished) {
            std::terminate();
        }
        events_.push_back(tunnel_data ? http2_event::tunnel_end(header_value.stream_id_)
                                      : http2_event::message_end(header_value.stream_id_));
        release_local_request_stream_if_closed(*stream);
    }
    return true;
}

}  // namespace ruvia::detail
