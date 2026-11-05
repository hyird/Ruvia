#include <algorithm>
#include <array>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/response/http_response_body_access.h"
#include "ruvia/http/detail/server/http_final_response_control_plan.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/http/http_request_content_semantics.h"
#include "ruvia/http/http_response_stream.h"

#include "http2/http2_connection.h"
#include "http2/http2_flow_control.h"
#include "http2/http2_header_list_size.h"
#include "http2/http2_header_rules.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_response_headers.h"
#include "http2/http2_websocket_handshake.h"
#include "response/http_response_content_semantics.h"
#include "util/http_text_overlap.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool http2_is_valid_connect_response_head(const http_response& response) noexcept {
    const auto& body = response_body(response);
    if (!response.status().is_successful() || body.size() != 0 || body.file().has_value()) {
        return false;
    }
    return !response_has_known_header(
        response, response_header_content_length | response_header_transfer_encoding);
}

}  // namespace

void http2_connection::append_response_header_frames(
    http2_stream_state& stream, std::string_view header_block, http2_end_stream end_stream,
    std::size_t additional_output_bytes, std::size_t additional_output_segments) {
    const auto output_checkpoint = output_.checkpoint();
    const auto previous_encoder_table_size_update_pending = encoder_table_size_update_pending_;
    try {
        // A HEADERS + CONTINUATION run must be an uninterrupted frame sequence for the same
        // stream (RFC 9113 §6.10). Appending them contiguously to the single outbound buffer
        // guarantees that ordering.
        const auto table_size_update = encoder_table_size_update_pending_
                                           ? encoder_zero_table_size_update_
                                           : std::string_view{};

        const std::size_t max_frame = peer_settings_.max_frame_size();
        const auto encoded_bytes =
            http2_header_frame_encoded_bytes(header_block.size(), max_frame, table_size_update.size());
        const auto encoded_segments = http2_header_frame_count(encoded_bytes, max_frame);
        if (additional_output_bytes > std::numeric_limits<std::size_t>::max() - encoded_bytes ||
            additional_output_segments > std::numeric_limits<std::size_t>::max() - encoded_segments) {
            throw std::length_error("HTTP/2 response output size overflow");
        }
        output_.reserve_additional(encoded_bytes + additional_output_bytes);
        output_.reserve_segments_additional(encoded_segments + additional_output_segments);

        std::size_t offset = 0;
        bool first = true;
        while (offset < header_block.size() || (first && !table_size_update.empty())) {
            const auto prefix = first ? table_size_update : std::string_view{};
            const auto chunk =
                std::min<std::size_t>(header_block.size() - offset, max_frame - prefix.size());
            const bool last = offset + chunk == header_block.size();
            const auto flags = static_cast<std::uint8_t>(
                (last ? http2_flag_end_headers : 0) |
                (first && http2_ends_stream(end_stream) ? http2_flag_end_stream : 0));
            output_.append_frame(first ? http2_frame_type::headers : http2_frame_type::continuation,
                flags, stream.id(), prefix, header_block.substr(offset, chunk));
            offset += chunk;
            first = false;
        }
        if (!table_size_update.empty()) {
            encoder_table_size_update_pending_ = false;
        }
    } catch (...) {
        // Header submission is one transaction. A throwing PMR resource must not leave
        // a partial frame run, consume a pending HPACK table-size update, or retain a
        // staged block that no longer has a matching stream state.
        output_.rollback_to(output_checkpoint);
        encoder_table_size_update_pending_ = previous_encoder_table_size_update_pending;
        local_header_block_.clear();
        throw;
    }
    if (stream.push_reservation() == http2_push_reservation::local && stream.hold_peer_concurrency_slot()) {
        ++active_local_request_streams_;
    }
    stream.activate_push();
}

void http2_connection::commit_connect_response_head(
    http2_stream_state& stream, bool terminal_remote_half) {
    append_response_header_frames(
        stream, std::string_view(local_header_block_), http2_end_stream::keep_open);
    (void)stream.accept_connect();
    stream.begin_local_content_unbounded();
    (void)stream.open_local_connect_tunnel();
    http2_release_local_header_block(local_header_block_);
    if (terminal_remote_half) {
        events_.push_back(http2_event(http2_tunnel_end_event(stream.id())));
    }
}

std::variant<http_buffered_response_write_plan, http2_response_head_submit_error>
http2_connection::submit_response_head(
    std::uint32_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_response_head_submit_error::closed;
    }
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) ||
        stream->local_send().head_pending() == nullptr) {
        return http2_response_head_submit_error::invalid_state;
    }
    if (write_plan.body_plan().request_method() != stream->request_known_method() ||
        !write_plan.matches_response(response)) {
        return http2_response_head_submit_error::response_plan_mismatch;
    }
    if (stream->push_reservation() == http2_push_reservation::local &&
        active_local_request_streams_ >= peer_settings_.max_concurrent_streams()) {
        return http2_response_head_submit_error::peer_stream_limit_reached;
    }
    const bool successful_connect =
        response.status().is_successful() && stream->tunnel().pending() != nullptr;
    if (successful_connect) {
        return http2_response_head_submit_error::invalid_state;
    }
    if (http2_final_response_control_plan(response).has_value()) {
        return http2_response_head_submit_error::invalid_message;
    }

    const auto head_plan_result = http2_buffered_response_head_plan(write_plan, response);
    const auto* head_plan = std::get_if<http2_response_head_plan>(&head_plan_result);
    if (head_plan == nullptr) {
        const auto error = std::get<http2_response_head_plan_error>(head_plan_result);
        const bool response_plan_mismatch =
            error == http2_response_head_plan_error::response_status_mismatch ||
            error == http2_response_head_plan_error::response_representation_mismatch;
        return response_plan_mismatch
                   ? http2_response_head_submit_error::response_plan_mismatch
                   : http2_response_head_submit_error::invalid_message;
    }
    if (!append_http2_response_headers(local_header_block_, response, *head_plan)) {
        return http2_response_head_submit_error::invalid_message;
    }
    const auto end_stream =
        write_plan.send_body() ? http2_end_stream::keep_open : http2_end_stream::end_stream;
    append_response_header_frames(*stream, std::string_view(local_header_block_), end_stream);
    if (head_plan->body_plan().body_suppressed()) {
        stream->begin_local_content_forbidden();
    } else {
        stream->begin_local_content_known_length(write_plan.content_length());
    }
    if (http2_ends_stream(end_stream)) {
        (void)stream->commit_local_head_end_stream();
    } else {
        (void)stream->begin_local_response_content();
    }
    if (stream->tunnel().pending() != nullptr) {
        (void)stream->reject_connect();
    }
    http2_release_local_header_block(local_header_block_);
    release_local_request_stream_if_closed(*stream);
    retire_completed_local_push(stream_id);
    return write_plan;
}

std::variant<http_response_stream_commit_plan, http2_response_head_submit_error>
http2_connection::submit_streaming_response_head(
    std::uint32_t stream_id, http_response head,
    http_response_trailer_intent trailer_intent) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_response_head_submit_error::closed;
    }
    if (stream->push_reservation() == http2_push_reservation::local &&
        active_local_request_streams_ >= peer_settings_.max_concurrent_streams()) {
        return http2_response_head_submit_error::peer_stream_limit_reached;
    }
    const bool successful_connect =
        head.status().is_successful() && stream->tunnel().pending() != nullptr;
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) ||
        stream->local_send().head_pending() == nullptr || successful_connect) {
        return http2_response_head_submit_error::invalid_state;
    }
    auto prepared_commit_plan = plan_http_response_stream_commit(http_response_stream_framing::http2_frames,
        stream->request_known_method(), head.status(), trailer_intent);
    if (!prepared_commit_plan.trailer_intent_allowed()) {
        return http2_response_head_submit_error::invalid_message;
    }
    if (http2_final_response_control_plan(head).has_value()) {
        return http2_response_head_submit_error::invalid_message;
    }
    auto stream_head =
        prepare_http_response_stream_head(std::move(head), std::move(prepared_commit_plan));
    const auto& commit_plan = stream_head.commit_plan();
    // One prepared plan owns both the encoded Content-Length metadata and the
    // local DATA accounting contract. Explicit length is parsed exactly once;
    // absence remains unbounded, while content-forbidden responses never become
    // DATA-open.
    const auto head_plan_result =
        http2_streaming_response_head_plan(commit_plan.body_plan(), stream_head.response());
    const auto* head_plan = std::get_if<http2_response_head_plan>(&head_plan_result);
    if (head_plan == nullptr) {
        return http2_response_head_submit_error::invalid_message;
    }
    if (!append_http2_response_headers(local_header_block_, stream_head.response(), *head_plan)) {
        return http2_response_head_submit_error::invalid_message;
    }
    const auto end_stream =
        commit_plan.head_disposition() == http_response_stream_head_disposition::message_ended
            ? http2_end_stream::end_stream
            : http2_end_stream::keep_open;
    append_response_header_frames(*stream, std::string_view(local_header_block_), end_stream);
    if (head_plan->body_plan().body_suppressed()) {
        stream->begin_local_content_forbidden();
    } else if (const auto content_length = head_plan->streaming_content_length()) {
        stream->begin_local_content_known_length(*content_length);
    } else {
        stream->begin_local_content_unbounded();
    }
    if (http2_ends_stream(end_stream)) {
        (void)stream->commit_local_head_end_stream();
    } else {
        (void)stream->begin_local_response_content();
    }
    if (stream->tunnel().pending() != nullptr) {
        (void)stream->reject_connect();
    }
    http2_release_local_header_block(local_header_block_);
    release_local_request_stream_if_closed(*stream);
    retire_completed_local_push(stream_id);
    return commit_plan;
}

http2_submit_status http2_connection::submit_interim_response_head(
    std::uint32_t stream_id, const http_interim_response_head& response) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_submit_status::closed;
    }
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) ||
        stream->local_send().head_pending() == nullptr) {
        return http2_submit_status::invalid_state;
    }
    if (stream->push_reservation() == http2_push_reservation::local &&
        active_local_request_streams_ >= peer_settings_.max_concurrent_streams()) {
        return http2_submit_status::peer_capability_unavailable;
    }
    if (append_http2_interim_response_headers(local_header_block_, response) !=
        http2_interim_response_header_encode_status::ok) {
        return http2_submit_status::invalid_message;
    }
    append_response_header_frames(
        *stream, std::string_view(local_header_block_), http2_end_stream::keep_open);
    http2_release_local_header_block(local_header_block_);
    return http2_submit_status::accepted;
}

http2_data_submit_result http2_connection::submit_data(std::uint32_t stream_id,
    std::string_view chunk, http2_end_stream end_stream, std::size_t max_payload_bytes) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return {0, http2_data_submit_status::closed};
    }
    const auto& local_send = stream->local_send();
    if (local_send.request_content_open() == nullptr && local_send.response_content_open() == nullptr &&
        local_send.tunnel_open() == nullptr) {
        throw std::logic_error("HTTP/2 stream is not open for DATA submission");
    }
    if (local_send.request_content_open() != nullptr) {
        if (stream->request_continue_pending()) {
            return {0, http2_data_submit_status::expectation_pending};
        }
        if (stream->request_content_canceled()) {
            throw std::logic_error("HTTP/2 request content was canceled");
        }
    }
    // Check the whole offered input before consuming a prefix. A terminal input
    // that cannot satisfy Content-Length must never partially reach the wire.
    switch (stream->local_content().check_accept(chunk.size(), http2_ends_stream(end_stream))) {
        case http2_local_content_check::accepted:
            break;
        case http2_local_content_check::not_started:
        case http2_local_content_check::forbidden:
            throw std::logic_error("HTTP/2 stream content is not allowed");
        case http2_local_content_check::length_exceeded:
            throw std::invalid_argument("HTTP/2 DATA exceeds declared Content-Length");
        case http2_local_content_check::length_incomplete:
            throw std::invalid_argument("HTTP/2 DATA ended before declared Content-Length");
    }
    const auto consumed = std::min({chunk.size(), max_payload_bytes,
        http2_available_send_window(connection_send_window_, *stream)});
    const bool complete = consumed == chunk.size();
    const auto terminal = complete ? end_stream : http2_end_stream::keep_open;
    if (consumed != 0 || (chunk.empty() && http2_ends_stream(terminal))) {
        const auto output_bytes = consumed == 0 ? http2_frame_header_bytes
                                                : http2_data_frame_encoded_bytes(consumed, peer_settings_.max_frame_size());
        const auto output_segments = consumed == 0 ? std::size_t{1}
                                                   : http2_data_frame_count(consumed, peer_settings_.max_frame_size());
        // Reserve all frame bytes and metadata before the first mutation. Frame
        // appends below cannot allocate; no caller-owned suffix is copied or retained.
        output_.reserve_additional(output_bytes);
        output_.reserve_segments_additional(output_segments);
        if (consumed == 0) {
            output_.append_frame(http2_frame_type::data, http2_flag_end_stream, stream_id, {});
        } else {
            append_data_frames(*stream, chunk.substr(0, consumed), terminal);
        }
        if (http2_ends_stream(terminal)) {
            (void)stream->commit_local_end_stream();
            release_local_request_stream_if_closed(*stream);
        }
        retire_completed_local_push(stream_id);
    }
    return {consumed, complete ? http2_data_submit_status::complete
                               : http2_data_submit_status::backpressured};
}

http2_request_content_release_status http2_connection::release_request_content(
    std::uint32_t stream_id) noexcept {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_request_content_release_status::closed;
    }
    return stream->release_request_continue() ? http2_request_content_release_status::released
                                              : http2_request_content_release_status::not_pending;
}

http2_submit_status http2_connection::submit_connect_response_head(
    std::uint32_t stream_id, const http_response& response) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_submit_status::closed;
    }
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) ||
        stream->local_send().head_pending() == nullptr || stream->tunnel().pending() == nullptr) {
        return http2_submit_status::invalid_state;
    }
    if (!http2_is_valid_connect_response_head(response)) {
        return http2_submit_status::invalid_message;
    }
    if (http2_final_response_control_plan(response).has_value()) {
        return http2_submit_status::invalid_message;
    }
    const auto head_plan_result = http2_connect_response_head_plan(
        plan_http_response_body(http_known_method::connect, response.status()));
    const auto* head_plan = std::get_if<http2_response_head_plan>(&head_plan_result);
    if (head_plan == nullptr) {
        return http2_submit_status::invalid_message;
    }
    const bool terminal_remote_half = http2_remote_peer_half_closed(*stream);
    if (terminal_remote_half) {
        reserve_event_slots(1);
    }
    if (!append_http2_response_headers(local_header_block_, response, *head_plan)) {
        return http2_submit_status::invalid_message;
    }
    commit_connect_response_head(*stream, terminal_remote_half);
    return http2_submit_status::accepted;
}

std::variant<http2_websocket_negotiation, http2_websocket_handshake_submit_error>
http2_connection::submit_websocket_handshake(
    std::uint32_t stream_id, const websocket_handshake_validation_result& validation,
    websocket_server_negotiation&& negotiation) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_websocket_handshake_submit_error::closed;
    }
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) ||
        http2_websocket_connect_remote_head_ended(*stream) ||
        !http2_websocket_connect_content_acceptable(*stream) ||
        stream->local_send().head_pending() == nullptr ||
        !http2_is_pending_websocket_connect(*stream) || validation.accepted() == nullptr) {
        return http2_websocket_handshake_submit_error::invalid_state;
    }
    const bool terminal_remote_half = http2_remote_peer_half_closed(*stream);
    if (terminal_remote_half) {
        reserve_event_slots(1);
    }
    http2_encode_websocket_handshake_headers(local_header_block_, negotiation);
    commit_connect_response_head(*stream, terminal_remote_half);
    const auto compression = negotiation.compression();
    return http2_websocket_negotiation(std::move(negotiation).take_subprotocol(), compression);
}

http2_finish_status http2_connection::finish_request(std::uint32_t stream_id,
    std::span<const http_header_view> trailers) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_finish_status::closed;
    }
    if (role_ != http2_role::client || stream->local_send().request_content_open() == nullptr ||
        stream->request_continue_pending() || stream->request_content_canceled()) {
        throw std::logic_error("HTTP/2 stream is not open for finishing request");
    }
    if (!stream->local_content().length_complete()) {
        throw std::invalid_argument("HTTP/2 request ended before declared Content-Length");
    }
    if (trailers.empty()) {
        output_.append_frame(http2_frame_type::data, http2_flag_end_stream, stream_id, {});
    } else {
        http2_header_list_size size;
        std::size_t reserve_bytes = 0;
        for (const auto& field : trailers) {
            if (!http2_is_valid_outbound_regular_header(field.name(), field.value()) ||
                is_forbidden_http_request_trailer_name(field.name()) || !size.add(field.name(), field.value())) {
                throw std::invalid_argument("invalid HTTP/2 request trailer");
            }
            reserve_bytes += field.name().size() + field.value().size() + 6U;
        }
        auto& block = local_header_block_;
        try {
            block.clear();
            if (reserve_bytes > 15U) {
                block.reserve(reserve_bytes);
            }
            std::array<char, http2_lower_header_stack_bytes> lower_name_stack;
            std::pmr::string lower_name_scratch(resource_);
            for (const auto& field : trailers) {
                hpack_encoder::encode_header(block,
                    http2_lower_header_name(field.name(), lower_name_stack, lower_name_scratch),
                    field.value());
            }
        } catch (...) {
            block.clear();
            throw;
        }
        append_response_header_frames(*stream, std::string_view(block), http2_end_stream::end_stream);
        http2_release_local_header_block(local_header_block_);
    }
    (void)stream->commit_local_end_stream();
    release_local_request_stream_if_closed(*stream);
    return http2_finish_status::accepted;
}

http2_finish_status http2_connection::finish_response(
    std::uint32_t stream_id, const http_response_trailer_section& trailers) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_finish_status::closed;
    }
    if (stream->local_send().response_content_open() == nullptr) {
        throw std::logic_error("HTTP/2 stream is not open for finishing response");
    }
    if (!stream->local_content().length_complete()) {
        throw std::invalid_argument("HTTP/2 response ended before declared Content-Length");
    }
    // Encode the complete semantic trailer section before output or phase changes.
    // Body input must already be consumed; DATA and terminal HEADERS preserve order.
    if (trailers.empty()) {
        output_.append_frame(http2_frame_type::data, http2_flag_end_stream, stream_id, {});
    } else {
        auto& trailer_block = local_header_block_;
        try {
            trailer_block.clear();
            append_http2_response_trailers(trailer_block, trailers);
        } catch (...) {
            trailer_block.clear();
            throw;
        }
        append_response_header_frames(*stream, std::string_view(trailer_block), http2_end_stream::end_stream);
        http2_release_local_header_block(local_header_block_);
    }
    (void)stream->commit_local_end_stream();
    release_local_request_stream_if_closed(*stream);
    retire_completed_local_push(stream_id);
    return http2_finish_status::accepted;
}

http2_submit_status http2_connection::submit_reset(std::uint32_t stream_id, http2_error_code error) {
    if (stream_id == 0) {
        return http2_submit_status::invalid_state;
    }
    auto* stream = find_stream(stream_id);
    if (stream == nullptr) {
        const auto close_source = closed_streams_.source(stream_id);
        if (!close_source.has_value()) {
            return http2_submit_status::invalid_state;
        }
        if (error == http2_error_code::protocol_error &&
            close_source == http2_stream_close_source::local &&
            local_connection_state_.fatal_failure() == nullptr &&
            !closed_streams_.local_reset_sent(stream_id)) {
            // append_rst_stream publishes one complete frame or leaves output
            // unchanged. Consume the one-shot late reset only after it succeeds.
            output_.append_rst_stream(stream_id, error);
            closed_streams_.remember(stream_id, http2_stream_close_source::local, true);
            return http2_submit_status::accepted;
        }
        return http2_submit_status::closed;
    }
    if (stream->is_aborted()) {
        return http2_submit_status::closed;
    }
    if (local_connection_state_.fatal_failure() != nullptr) {
        // Owner release still retires the stream and its leases after failure;
        // only the wire reset is suppressed on the terminal connection.
        return close_stream_by_owner(stream_id) ? http2_submit_status::accepted
                                                : http2_submit_status::closed;
    }
    // A client-created stream is still RFC-idle until its request HEADERS are
    // submitted; RST_STREAM on that state would make the peer close the connection.
    // A server owner does not own a peer stream until its initial header block has
    // decoded; rejecting an early reset also preserves the mandatory CONTINUATION run.
    if ((role_ == http2_role::client && stream->local_send().head_pending() != nullptr) ||
        (role_ == http2_role::server && !http2_remote_final_head_decoded(*stream)) ||
        (http2_stream_is_closed(*stream) && error != http2_error_code::protocol_error)) {
        return http2_submit_status::invalid_state;
    }
    return reset_stream(stream_id, error, close_notification_type::owner_already_knows)
               ? http2_submit_status::accepted
               : http2_submit_status::closed;
}

bool http2_connection::reset_stream(std::uint32_t stream_id, http2_error_code error,
    close_notification_type notification) {
    const auto output_checkpoint = output_.checkpoint();
    try {
        output_.append_rst_stream(stream_id, error);
        // A reset returns no send credit, so it cannot unblock other streams.
        return close_stream_impl(stream_id, http2_stream_close_source::local, error, notification);
    } catch (...) {
        output_.rollback_to(output_checkpoint);
        throw;
    }
}

}  // namespace ruvia::detail
