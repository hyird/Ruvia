#include <algorithm>
#include <array>
#include <limits>
#include <memory_resource>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/coding/http_response_content_semantics.h"
#include "ruvia/http/detail/field/http_header_section_size.h"
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
#include "http2/http2_header_rules.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_response_headers.h"
#include "http2/http2_websocket_handshake.h"
#include "util/http_text_overlap.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool http2_is_valid_connect_response_head(const http_response& response) noexcept {
    const auto& body = response_body(response);
    if (!response.status().is_successful() || body.size() != 0 || body.file().has_value()) {
        return false;
    }
    for (const auto& header : response.headers()) {
        if (http_field_name_in(header.name(), http_message_framing_fields)) {
            return false;
        }
    }
    return true;
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
        stream.local_header_block().clear();
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
        stream, std::string_view(stream.local_header_block()), http2_end_stream::keep_open);
    (void)stream.accept_connect();
    stream.begin_local_content_unbounded();
    (void)stream.open_local_connect_tunnel();
    http2_release_local_header_block(stream);
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
    if (write_plan.request_method() != stream->request_known_method() ||
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
    if (!append_http2_response_headers(*stream, response, *head_plan)) {
        return http2_response_head_submit_error::invalid_message;
    }
    std::optional<std::pmr::string> retired_header_block;
    if (stream->local_header_block().capacity() > http_retained_pmr_string_bytes) {
        retired_header_block.emplace(stream->local_header_block().get_allocator());
    }
    const auto end_stream =
        write_plan.send_body() ? http2_end_stream::keep_open : http2_end_stream::end_stream;
    append_response_header_frames(*stream, std::string_view(stream->local_header_block()), end_stream);
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
    if (retired_header_block) {
        stream->local_header_block().swap(*retired_header_block);
    } else {
        stream->local_header_block().clear();
    }
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
    if (!append_http2_response_headers(*stream, stream_head.response(), *head_plan)) {
        return http2_response_head_submit_error::invalid_message;
    }
    const auto end_stream =
        commit_plan.head_disposition() == http_response_stream_head_disposition::message_ended
            ? http2_end_stream::end_stream
            : http2_end_stream::keep_open;
    append_response_header_frames(*stream, std::string_view(stream->local_header_block()), end_stream);
    if (head_plan->body_plan().body_suppressed()) {
        stream->begin_local_content_forbidden();
    } else if (const auto content_length = head_plan->streaming_content_length()) {
        stream->begin_local_content_known_length(*content_length);
    } else {
        stream->begin_local_content_unbounded();
    }
    if (commit_plan.head_disposition() == http_response_stream_head_disposition::trailers_only) {
        (void)stream->begin_local_response_trailers_only();
    } else {
        if (http2_ends_stream(end_stream)) {
            (void)stream->commit_local_head_end_stream();
        } else {
            (void)stream->begin_local_response_content();
        }
    }
    if (stream->tunnel().pending() != nullptr) {
        (void)stream->reject_connect();
    }
    http2_release_local_header_block(*stream);
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
    if (append_http2_interim_response_headers(*stream, response) !=
        http2_interim_response_header_encode_status::ok) {
        return http2_submit_status::invalid_message;
    }
    append_response_header_frames(
        *stream, std::string_view(stream->local_header_block()), http2_end_stream::keep_open);
    http2_release_local_header_block(*stream);
    return http2_submit_status::accepted;
}

http2_data_submit_status http2_connection::submit_data(
    std::uint32_t stream_id, std::string_view chunk, http2_end_stream end_stream) {
    auto* stream = find_outbound_stream(stream_id);
    if (stream == nullptr) {
        return http2_data_submit_status::closed;
    }
    const auto& local_send = stream->local_send();
    if (local_send.request_content_open() == nullptr && local_send.response_content_open() == nullptr &&
        local_send.tunnel_open() == nullptr) {
        throw std::logic_error("HTTP/2 stream is not open for DATA submission");
    }
    if (local_send.request_content_open() != nullptr) {
        if (stream->request_continue_pending()) {
            return http2_data_submit_status::expectation_pending;
        }
        if (stream->request_content_canceled()) {
            throw std::logic_error("HTTP/2 request content was canceled");
        }
    }
    prepared_data_submission prepared;
    const auto status = prepare_data_submission(*stream, chunk, end_stream, stream->local_content(), prepared);
    if (status != http2_data_submit_status::accepted) {
        return status;
    }
    if (prepared.output_bytes_ != 0) {
        output_.reserve_additional(prepared.output_bytes_);
        output_.reserve_segments_additional(prepared.output_segments_);
    }
    return commit_data_submission(*stream, prepared);
}

http2_data_submit_status http2_connection::prepare_data_submission(http2_stream_state& stream,
    std::string_view chunk, http2_end_stream end_stream, const http2_local_content_state& content,
    prepared_data_submission& prepared) {
    // One queued submission per stream is the hard backpressure boundary. The
    // current input remains caller-owned and can be retried after the prior one drains.
    for (const auto& pending : pending_sends_) {
        if (pending.stream_id_ == stream.id()) {
            return http2_data_submit_status::backpressured;
        }
    }
    switch (content.check_accept(chunk.size(), http2_ends_stream(end_stream))) {
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
    prepared.input_ = chunk;
    prepared.end_stream_ = end_stream;
    // Prepare every allocation needed for a deferred suffix BEFORE accepting the
    // input or consuming flow-control window. A recoverable allocation failure can
    // therefore never leave a framed prefix without its core-owned remainder.
    std::size_t immediate_bytes = 0;
    if (!chunk.empty()) {
        immediate_bytes =
            std::min(chunk.size(), http2_available_send_window(connection_send_window_, stream));
        if (immediate_bytes < chunk.size()) {
            std::pmr::string remainder(resource_);
            remainder.append(chunk.data() + immediate_bytes, chunk.size() - immediate_bytes);
            if (pending_sends_.size() == pending_sends_.capacity()) {
                const auto max_size = pending_sends_.max_size();
                if (pending_sends_.size() == max_size) {
                    throw std::length_error("HTTP/2 pending send queue size overflow");
                }
                const auto required = pending_sends_.size() + 1;
                const auto doubled = pending_sends_.capacity() > max_size / 2
                                         ? max_size
                                         : pending_sends_.capacity() * 2;
                pending_sends_.reserve(std::max(required, doubled));
            }
            prepared.deferred_.emplace(http2_pending_send{
                stream.id(), std::move(remainder), 0, end_stream, std::pmr::string(resource_)});
        }
    }
    if (immediate_bytes != 0 || (chunk.empty() && http2_ends_stream(end_stream))) {
        prepared.output_bytes_ = immediate_bytes == 0 ? http2_frame_header_bytes
                                                      : http2_data_frame_encoded_bytes(immediate_bytes,
                                                            peer_settings_.max_frame_size());
        prepared.output_segments_ = immediate_bytes == 0
                                        ? 1
                                        : http2_data_frame_count(immediate_bytes, peer_settings_.max_frame_size());
    }
    return http2_data_submit_status::accepted;
}

http2_data_submit_status http2_connection::commit_data_submission(
    http2_stream_state& stream, prepared_data_submission& prepared) {
    const auto stream_id = stream.id();
    const auto chunk = prepared.input_;
    const auto end_stream = prepared.end_stream_;
    // Accepted means ownership of the WHOLE input, even when flow control below
    // can only materialize a prefix and the prepared suffix becomes pending.
    stream.accept_local_content(chunk.size());
    if (chunk.empty()) {
        if (http2_ends_stream(end_stream)) {
            output_.append_frame(http2_frame_type::data, http2_flag_end_stream, stream_id, {});
            (void)stream.commit_local_end_stream();
            release_local_request_stream_if_closed(stream);
        }
        retire_completed_local_push(stream_id);
        return http2_data_submit_status::accepted;
    }
    const auto consumed = send_data_up_to_window(stream, chunk, 0, end_stream);
    if (consumed < chunk.size()) {
        // Preparation used these same windows and reserved the exact suffix and
        // queue slot, so committing ownership cannot allocate here.
        pending_sends_.push_back(std::move(*prepared.deferred_));
        if (http2_ends_stream(end_stream)) {
            (void)stream.queue_local_end_stream();
        }
        return http2_data_submit_status::queued;
    }
    if (http2_ends_stream(end_stream)) {
        (void)stream.commit_local_end_stream();
        release_local_request_stream_if_closed(stream);
    }
    retire_completed_local_push(stream_id);
    return http2_data_submit_status::accepted;
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
    if (!append_http2_response_headers(*stream, response, *head_plan)) {
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
    http2_encode_websocket_handshake_headers(stream->local_header_block(), negotiation);
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
    http_header_section_size size;
    for (const auto& field : trailers) {
        if (!http2_is_valid_outbound_regular_header(field.name(), field.value()) ||
            is_forbidden_http_request_trailer_name(field.name()) || !size.add(field.name(), field.value())) {
            throw std::invalid_argument("invalid HTTP/2 request trailer");
        }
    }
    std::pmr::string block(resource_);
    std::array<char, http2_lower_header_stack_bytes> lower_name_stack{};
    std::pmr::string lower_name_scratch(resource_);
    for (const auto& field : trailers) {
        hpack_encoder::encode_header(block,
            http2_lower_header_name(field.name(), lower_name_stack, lower_name_scratch),
            field.value());
    }
    for (auto& pending : pending_sends_) {
        if (pending.stream_id_ == stream_id) {
            pending.end_stream_ = block.empty() ? http2_end_stream::end_stream : http2_end_stream::keep_open;
            pending.trailer_block_.swap(block);
            (void)stream->queue_local_end_stream();
            return http2_finish_status::queued;
        }
    }
    if (block.empty()) {
        output_.append_frame(http2_frame_type::data, http2_flag_end_stream, stream_id, {});
    } else {
        append_response_header_frames(*stream, block, http2_end_stream::end_stream);
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
    if (stream->local_send().response_content_open() == nullptr &&
        stream->local_send().response_trailers_only() == nullptr) {
        throw std::logic_error("HTTP/2 stream is not open for finishing response");
    }
    if (!stream->local_content().length_complete()) {
        throw std::invalid_argument("HTTP/2 response ended before declared Content-Length");
    }
    if (stream->local_send().response_trailers_only() != nullptr && trailers.empty()) {
        // A trailers-only response cannot fall back to DATA(END_STREAM): its
        // method/status explicitly forbids DATA, including an empty terminal frame.
        throw std::logic_error("HTTP/2 trailers-only response requires non-empty trailers");
    }
    // The entire semantic trailer section was validated before the initial head
    // commit and is encoded in detached
    // storage before output, pending DATA, or stream phase changes. It either joins
    // the terminal transaction whole or leaves no per-stream staged side channel.
    std::pmr::string trailer_block(resource_);
    append_http2_response_trailers(trailer_block, trailers);
    // If the body still has a window-blocked remainder, the trailer HEADERS must NOT
    // jump ahead of that queued DATA. Stash it on the pending entry and move END_STREAM
    // from the body to the trailer (mark_send_window_opened emits it once the body drains).
    for (auto& pending : pending_sends_) {
        if (pending.stream_id_ == stream_id) {
            if (trailer_block.empty()) {
                pending.end_stream_ = http2_end_stream::end_stream;
            } else {
                pending.end_stream_ = http2_end_stream::keep_open;
                pending.trailer_block_.swap(trailer_block);
            }
            (void)stream->queue_local_end_stream();
            return http2_finish_status::queued;
        }
    }
    if (trailer_block.empty()) {
        output_.append_frame(http2_frame_type::data, http2_flag_end_stream, stream_id, {});
    } else {
        append_response_header_frames(*stream, std::string_view(trailer_block), http2_end_stream::end_stream);
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
            closed_streams_.claim_late_local_reset(stream_id)) {
            const auto output_checkpoint = output_.checkpoint();
            try {
                output_.append_rst_stream(stream_id, error);
                return http2_submit_status::accepted;
            } catch (...) {
                output_.rollback_to(output_checkpoint);
                throw;
            }
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
    const auto output_checkpoint = output_.checkpoint();
    try {
        output_.append_rst_stream(stream_id, error);
        return close_stream_by_owner(stream_id) ? http2_submit_status::accepted
                                                : http2_submit_status::closed;
    } catch (...) {
        output_.rollback_to(output_checkpoint);
        throw;
    }
}

}  // namespace ruvia::detail
