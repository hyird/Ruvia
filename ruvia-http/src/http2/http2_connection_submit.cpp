#include <algorithm>
#include <array>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/response/http_response_body_access.h"
#include "ruvia/http/detail/server/http_final_response_control_plan.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/detail/util/http_address_overlap.h"
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

void http2_connection::append_field_block_frames(
    std::uint32_t stream_id, std::optional<std::uint32_t> promised_stream_id, std::uint8_t first_flags) {
    const std::string_view block(local_header_block_);
    const auto output_checkpoint = output_.checkpoint();
    try {
        // The first frame carries the PUSH_PROMISE promised stream id, then a pending
        // dynamic table size update, which must open the field block (RFC 7541 §4.2).
        // Every frame but the last is full, so the split points depend only on the
        // total payload.
        constexpr std::size_t promised_stream_id_bytes = 4;
        std::array<char, promised_stream_id_bytes + encoder_zero_table_size_update_.size()> first_prefix_bytes;
        std::size_t first_prefix_size = 0;
        if (promised_stream_id.has_value()) {
            http2_write32(first_prefix_bytes.data(), *promised_stream_id);
            first_prefix_size = promised_stream_id_bytes;
        }
        if (encoder_table_size_update_pending_) {
            std::ranges::copy(encoder_zero_table_size_update_, first_prefix_bytes.begin() + first_prefix_size);
            first_prefix_size += encoder_zero_table_size_update_.size();
        }

        const std::size_t max_frame = peer_settings_.max_frame_size();
        const auto encoded_bytes = http2_header_frame_encoded_bytes(first_prefix_size + block.size(), max_frame);
        // A HEADERS or PUSH_PROMISE + CONTINUATION run must be an uninterrupted frame
        // sequence (RFC 9113 §6.10); reserving it whole keeps the appends below atomic.
        output_.reserve_additional(encoded_bytes);
        output_.reserve_segments_additional(http2_header_frame_count(encoded_bytes, max_frame));

        auto type = promised_stream_id.has_value() ? http2_frame_type::push_promise : http2_frame_type::headers;
        auto flags = first_flags;
        std::string_view prefix(first_prefix_bytes.data(), first_prefix_size);
        std::size_t offset = 0;
        do {
            const auto chunk = std::min(block.size() - offset, max_frame - prefix.size());
            const auto fragment = block.substr(offset, chunk);
            offset += chunk;
            const auto end_headers = offset == block.size() ? http2_flag_end_headers : std::uint8_t{0};
            output_.append_frame(type, static_cast<std::uint8_t>(flags | end_headers), stream_id, prefix, fragment);
            type = http2_frame_type::continuation;
            flags = 0;
            prefix = {};
        } while (offset < block.size());
    } catch (...) {
        // Header submission is one transaction. A throwing PMR resource must not leave
        // a partial frame run, consume a pending HPACK table-size update, or retain a
        // staged block that no longer has a matching stream state.
        output_.rollback_to(output_checkpoint);
        local_header_block_.clear();
        throw;
    }
    encoder_table_size_update_pending_ = false;
}

void http2_connection::append_header_frames(http2_stream_state& stream, http2_end_stream end_stream) {
    append_field_block_frames(
        stream.id(), std::nullopt, http2_ends_stream(end_stream) ? http2_flag_end_stream : std::uint8_t{0});
    if (stream.push_reservation() == http2_push_reservation::local && stream.hold_peer_concurrency_slot()) {
        ++active_local_request_streams_;
    }
    stream.activate_push();
}

void http2_connection::commit_connect_response_head(
    http2_stream_state& stream, bool terminal_remote_half) {
    append_header_frames(stream, http2_end_stream::keep_open);
    (void)stream.accept_connect();
    stream.begin_local_content_unbounded();
    (void)stream.open_local_connect_tunnel();
    http2_release_local_header_block(local_header_block_);
    if (terminal_remote_half) {
        publish_event(http2_event(http2_tunnel_end_event(stream.id())));
    }
}

std::optional<http2_response_head_submit_error> http2_connection::response_head_admission_error(
    const http2_stream_state* stream, http_status_code status) const noexcept {
    if (stream == nullptr) {
        return http2_response_head_submit_error::closed;
    }
    // A 2xx answer to a pending CONNECT opens the tunnel; only the CONNECT and
    // WebSocket handshake submissions commit one.
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) ||
        stream->local_send().head_pending() == nullptr ||
        (status.is_successful() && stream->tunnel().pending() != nullptr)) {
        return http2_response_head_submit_error::invalid_state;
    }
    if (stream->push_reservation() == http2_push_reservation::local &&
        active_local_request_streams_ >= peer_settings_.max_concurrent_streams()) {
        return http2_response_head_submit_error::peer_stream_limit_reached;
    }
    return std::nullopt;
}

bool http2_connection::commit_final_response_head(http2_stream_state& stream, const http_response& response,
    const http2_response_head_plan& head_plan, http2_end_stream end_stream,
    std::optional<std::uint64_t> content_length) {
    if (!append_http2_response_headers(local_header_block_, response, head_plan)) {
        return false;
    }
    const auto stream_id = stream.id();
    append_header_frames(stream, end_stream);
    if (head_plan.body_plan().body_suppressed()) {
        stream.begin_local_content_forbidden();
    } else if (content_length.has_value()) {
        stream.begin_local_content_known_length(*content_length);
    } else {
        stream.begin_local_content_unbounded();
    }
    if (http2_ends_stream(end_stream)) {
        (void)stream.commit_local_head_end_stream();
    } else {
        (void)stream.begin_local_response_content();
    }
    if (stream.tunnel().pending() != nullptr) {
        (void)stream.reject_connect();
    }
    http2_release_local_header_block(local_header_block_);
    release_local_request_stream_if_closed(stream);
    retire_completed_local_push(stream_id);
    return true;
}

std::variant<http_buffered_response_write_plan, http2_response_head_submit_error>
http2_connection::submit_response_head(
    std::uint32_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    auto* stream = find_outbound_stream(stream_id);
    if (const auto error = response_head_admission_error(stream, response.status())) {
        return *error;
    }
    if (write_plan.body_plan().request_method() != stream->request_known_method() ||
        !write_plan.matches_response(response)) {
        return http2_response_head_submit_error::response_plan_mismatch;
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
    const auto end_stream =
        write_plan.send_body() ? http2_end_stream::keep_open : http2_end_stream::end_stream;
    if (!commit_final_response_head(*stream, response, *head_plan, end_stream, write_plan.content_length())) {
        return http2_response_head_submit_error::invalid_message;
    }
    return write_plan;
}

std::variant<http_response_stream_commit_plan, http2_response_head_submit_error>
http2_connection::submit_streaming_response_head(
    std::uint32_t stream_id, http_response head,
    http_response_trailer_intent trailer_intent) {
    auto* stream = find_outbound_stream(stream_id);
    if (const auto error = response_head_admission_error(stream, head.status())) {
        return *error;
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
    const auto end_stream =
        commit_plan.head_disposition() == http_response_stream_head_disposition::message_ended
            ? http2_end_stream::end_stream
            : http2_end_stream::keep_open;
    if (!commit_final_response_head(*stream, stream_head.response(), *head_plan, end_stream,
            head_plan->streaming_content_length())) {
        return http2_response_head_submit_error::invalid_message;
    }
    return commit_plan;
}

http2_submit_status http2_connection::submit_interim_response_head(
    std::uint32_t stream_id, const http_interim_response_head& response) {
    auto* stream = find_outbound_stream(stream_id);
    if (const auto error = response_head_admission_error(stream, response.status())) {
        switch (*error) {
            case http2_response_head_submit_error::closed:
                return http2_submit_status::closed;
            case http2_response_head_submit_error::peer_stream_limit_reached:
                return http2_submit_status::peer_capability_unavailable;
            default:
                return http2_submit_status::invalid_state;
        }
    }
    if (append_http2_interim_response_headers(local_header_block_, response) !=
        http2_interim_response_header_encode_status::ok) {
        return http2_submit_status::invalid_message;
    }
    append_header_frames(*stream, http2_end_stream::keep_open);
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
        for (const auto& field : trailers) {
            if (!http2_is_valid_outbound_regular_header(field.name(), field.value()) ||
                is_forbidden_http_request_trailer_name(field.name()) || !size.add(field.name(), field.value())) {
                throw std::invalid_argument("invalid HTTP/2 request trailer");
            }
        }
        auto& block = local_header_block_;
        try {
            block.clear();
            http2_reserve_field_block(block, 0, trailers);
            http2_encode_lowercase_fields(block, trailers);
        } catch (...) {
            block.clear();
            throw;
        }
        append_header_frames(*stream, http2_end_stream::end_stream);
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
        append_header_frames(*stream, http2_end_stream::end_stream);
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
    auto* stream = streams_.find(stream_id);
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
    return reset_stream(stream_id, error) ? http2_submit_status::accepted : http2_submit_status::closed;
}

}  // namespace ruvia::detail
