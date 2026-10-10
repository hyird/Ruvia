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
#include "ruvia/http/http_request_content_semantics.h"
#include "ruvia/http/http_response_stream.h"

#include "http2/http2_connection.h"
#include "http2/http2_flow_control.h"
#include "http2/http2_header_rules.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_response_headers.h"
#include "http2/http2_websocket_handshake.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] bool http2_is_valid_connect_response_head(const http_response& response) noexcept {
    const auto& body = response_body(response);
    if (!response.status().is_successful() || body.size() != 0 || body.file().has_value()) {
        return false;
    }
    for (const auto& header : response.headers()) {
        const auto known = response_header_known_bit(header);
        if (known == response_header_content_length || known == response_header_transfer_encoding ||
            http_ascii_equals_ignore_case(header.name(), "content-length") ||
            http_ascii_equals_ignore_case(header.name(), "transfer-encoding")) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::size_t http2_data_frame_encoded_bytes(
    std::size_t data_bytes, std::size_t max_frame_size) {
    if (data_bytes == 0) {
        return http2_frame_header_bytes;
    }
    const auto frame_count = data_bytes / max_frame_size + (data_bytes % max_frame_size == 0 ? 0 : 1);
    if (frame_count >
        (std::numeric_limits<std::size_t>::max() - data_bytes) / http2_frame_header_bytes) {
        throw std::length_error("HTTP/2 DATA output size overflow");
    }
    return data_bytes + frame_count * http2_frame_header_bytes;
}
}  // namespace

void http2_connection::append_response_header_frames(
    http2_stream_state& stream, std::string_view header_block, http2_end_stream end_stream) {
    const auto output_checkpoint = output_.checkpoint();
    const auto previous_encoder_table_size_update_pending = encoder_table_size_update_pending_;
    try {
        // A HEADERS + CONTINUATION run must be an uninterrupted frame sequence for the same
        // stream (RFC 9113 §6.10). Appending them contiguously to the single outbound buffer
        // guarantees that ordering.
        // The table-size update is at most six bytes. Keep this scratch on the
        // stack so a later output-allocation failure cannot occur after a drain has
        // already committed flow-control state.
        std::array<std::byte, 32> table_size_update_storage{};
        std::pmr::monotonic_buffer_resource table_size_update_resource(table_size_update_storage.data(),
            table_size_update_storage.size(), std::pmr::null_memory_resource());
        std::pmr::string table_size_update(&table_size_update_resource);
        if (encoder_table_size_update_pending_) {
            hpack_encoder::encode_dynamic_table_size_update(table_size_update, encoder_dynamic_table_size_);
        }

        const std::size_t max_frame = peer_settings_.max_frame_size();
        std::size_t encoded_bytes = 0;
        std::size_t encoded_segments = 0;
        std::size_t planned_offset = 0;
        bool planned_first = true;
        while (planned_offset < header_block.size() || (planned_first && !table_size_update.empty())) {
            const auto prefix_size = planned_first ? table_size_update.size() : std::size_t{0};
            const auto chunk =
                std::min<std::size_t>(header_block.size() - planned_offset, max_frame - prefix_size);
            const auto frame_bytes = http2_frame_header_bytes + prefix_size + chunk;
            if (frame_bytes > std::numeric_limits<std::size_t>::max() - encoded_bytes) {
                throw std::length_error("HTTP/2 header output size overflow");
            }
            encoded_bytes += frame_bytes;
            ++encoded_segments;
            planned_offset += chunk;
            planned_first = false;
        }
        output_.reserve_additional(encoded_bytes);
        output_.reserve_segments_additional(encoded_segments);

        std::size_t offset = 0;
        bool first = true;
        while (offset < header_block.size() || (first && !table_size_update.empty())) {
            const auto prefix = first ? std::string_view(table_size_update) : std::string_view{};
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
        events_.push_back(http2_event::tunnel_end(stream.id()));
    }
}

http2_response_head_submit_result http2_connection::submit_response_head(
    std::uint32_t stream_id, const http_response& response, http_buffered_response_write_plan write_plan) {
    auto* stream = find_stream(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return http2_response_head_submit_result::make_failure(http2_response_head_submit_error::closed);
    }
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) ||
        stream->local_send().head_pending() == nullptr) {
        return http2_response_head_submit_result::make_failure(http2_response_head_submit_error::invalid_state);
    }
    if (write_plan.request_method() != stream->request_known_method() ||
        !write_plan.matches_response(response)) {
        return http2_response_head_submit_result::make_failure(http2_response_head_submit_error::response_plan_mismatch);
    }
    if (stream->push_reservation() == http2_push_reservation::local &&
        active_local_request_streams_ >= peer_settings_.max_concurrent_streams()) {
        return http2_response_head_submit_result::make_failure(http2_response_head_submit_error::peer_stream_limit_reached);
    }
    const bool successful_connect =
        response.status().is_successful() && stream->tunnel().pending() != nullptr;
    if (successful_connect) {
        return http2_response_head_submit_result::make_failure(http2_response_head_submit_error::invalid_state);
    }
    const auto control_result = http2_final_response_control_plan(response);
    const auto* http2_control = control_result.control();
    if (http2_control == nullptr) {
        return http2_response_head_submit_result::make_failure(http2_response_head_submit_error::invalid_message);
    }

    const auto head_plan_result = http2_buffered_response_head_plan(write_plan, response);
    const auto* head_plan = head_plan_result.plan();
    if (head_plan == nullptr) {
        const auto error = head_plan_result.failure()->error();
        const bool response_plan_mismatch =
            error == http2_response_head_plan_error::response_status_mismatch ||
            error == http2_response_head_plan_error::response_representation_mismatch;
        return response_plan_mismatch
                   ? http2_response_head_submit_result::make_failure(http2_response_head_submit_error::response_plan_mismatch)
                   : http2_response_head_submit_result::make_failure(http2_response_head_submit_error::invalid_message);
    }
    if (!append_http2_response_headers(*stream, response, *head_plan, *http2_control)) {
        return http2_response_head_submit_result::make_failure(http2_response_head_submit_error::invalid_message);
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
    http2_release_local_header_block(*stream);
    release_local_request_stream_if_closed(*stream);
    retire_completed_local_push(stream_id);
    return http2_response_head_submit_result::make_submitted(std::move(write_plan));
}

http2_streaming_response_head_submit_result http2_connection::submit_streaming_response_head(
    std::uint32_t stream_id, http_response head, http_response_stream_kind kind,
    http_response_trailer_intent trailer_intent) {
    auto* stream = find_stream(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return http2_streaming_response_head_submit_result::make_failure(http2_response_head_submit_error::closed);
    }
    if (stream->push_reservation() == http2_push_reservation::local &&
        active_local_request_streams_ >= peer_settings_.max_concurrent_streams()) {
        return http2_streaming_response_head_submit_result::make_failure(http2_response_head_submit_error::peer_stream_limit_reached);
    }
    const bool successful_connect =
        head.status().is_successful() && stream->tunnel().pending() != nullptr;
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) ||
        stream->local_send().head_pending() == nullptr || successful_connect) {
        return http2_streaming_response_head_submit_result::make_failure(http2_response_head_submit_error::invalid_state);
    }
    auto prepared_commit_plan = plan_http_response_stream_commit(http_response_stream_framing::http2_frames,
        stream->request_known_method(), head.status(), trailer_intent);
    if (!prepared_commit_plan.trailer_intent_allowed()) {
        return http2_streaming_response_head_submit_result::make_failure(http2_response_head_submit_error::invalid_message);
    }
    const auto control_result = http2_final_response_control_plan(head);
    const auto* http2_control = control_result.control();
    if (http2_control == nullptr) {
        return http2_streaming_response_head_submit_result::make_failure(http2_response_head_submit_error::invalid_message);
    }
    auto stream_head =
        prepare_http_response_stream_head(std::move(head), kind, std::move(prepared_commit_plan));
    const auto& commit_plan = stream_head.commit_plan();
    // One prepared plan owns both the encoded Content-Length metadata and the
    // local DATA accounting contract. Explicit length is parsed exactly once;
    // absence remains unbounded, while content-forbidden responses never become
    // DATA-open.
    const auto head_plan_result =
        http2_streaming_response_head_plan(commit_plan.body_plan(), stream_head.response());
    const auto* head_plan = head_plan_result.plan();
    if (head_plan == nullptr) {
        return http2_streaming_response_head_submit_result::make_failure(http2_response_head_submit_error::invalid_message);
    }
    if (!append_http2_response_headers(*stream, stream_head.response(), *head_plan, *http2_control)) {
        return http2_streaming_response_head_submit_result::make_failure(http2_response_head_submit_error::invalid_message);
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
    return http2_streaming_response_head_submit_result::make_submitted(commit_plan);
}

http2_submit_status http2_connection::submit_interim_response_head(
    std::uint32_t stream_id, const http_interim_response_head& response) {
    auto* stream = find_stream(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
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
    auto* stream = find_stream(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return http2_data_submit_status::closed;
    }
    const auto& local_send = stream->local_send();
    if (local_send.request_content_open() == nullptr && local_send.response_content_open() == nullptr &&
        local_send.tunnel_open() == nullptr) {
        return http2_data_submit_status::invalid_state;
    }
    if (local_send.request_content_open() != nullptr) {
        if (stream->request_continue_pending()) {
            return http2_data_submit_status::expectation_pending;
        }
        if (stream->request_content_canceled()) {
            return http2_data_submit_status::invalid_state;
        }
    }
    // One queued submission per stream is the hard backpressure boundary. The
    // current input remains caller-owned and can be retried after the prior one drains.
    for (const auto& pending : pending_sends_) {
        if (pending.stream_id_ == stream_id) {
            return http2_data_submit_status::backpressured;
        }
    }
    switch (stream->check_local_content_accept(chunk.size(), http2_ends_stream(end_stream))) {
        case http2_local_content_check::accepted:
            break;
        case http2_local_content_check::not_started:
        case http2_local_content_check::forbidden:
            return http2_data_submit_status::invalid_state;
        case http2_local_content_check::length_exceeded:
            return http2_data_submit_status::content_length_exceeded;
        case http2_local_content_check::length_incomplete:
            return http2_data_submit_status::content_length_incomplete;
    }
    // Prepare every allocation needed for a deferred suffix BEFORE accepting the
    // input or consuming flow-control window. A recoverable allocation failure can
    // therefore never leave a framed prefix without its core-owned remainder.
    std::optional<http2_pending_send> deferred;
    std::size_t immediate_bytes = 0;
    if (!chunk.empty()) {
        immediate_bytes =
            std::min(chunk.size(), http2_available_send_window(connection_send_window_, *stream));
        if (immediate_bytes < chunk.size()) {
            std::pmr::string remainder(resource_);
            remainder.append(chunk.data() + immediate_bytes, chunk.size() - immediate_bytes);
            pending_sends_.reserve(pending_sends_.size() + 1);
            deferred.emplace(http2_pending_send{
                stream_id, std::move(remainder), 0, end_stream, std::pmr::string(resource_)});
        }
    }
    if (immediate_bytes != 0 || (chunk.empty() && http2_ends_stream(end_stream))) {
        output_.reserve_additional(immediate_bytes == 0 ? http2_frame_header_bytes
                                                        : http2_data_frame_encoded_bytes(immediate_bytes,
                                                              peer_settings_.max_frame_size()));
        output_.reserve_segments_additional(immediate_bytes == 0
                                                ? 1
                                                : immediate_bytes / peer_settings_.max_frame_size() +
                                                      (immediate_bytes % peer_settings_.max_frame_size() == 0 ? 0 : 1));
    }
    // Accepted means ownership of the WHOLE input, even when flow control below
    // can only materialize a prefix and the prepared suffix becomes pending.
    stream->accept_local_content(chunk.size());
    if (chunk.empty()) {
        if (http2_ends_stream(end_stream)) {
            output_.append_frame(http2_frame_type::data, http2_flag_end_stream, stream_id, {});
            (void)stream->commit_local_end_stream();
            release_local_request_stream_if_closed(*stream);
        }
        retire_completed_local_push(stream_id);
        return http2_data_submit_status::accepted;
    }
    const auto consumed = send_data_up_to_window(*stream, chunk, 0, end_stream);
    if (consumed < chunk.size()) {
        // immediate_bytes above is the exact total that send_data_up_to_window can
        // consume from the current windows, so a deferred value must exist here.
        pending_sends_.push_back(std::move(*deferred));
        if (http2_ends_stream(end_stream)) {
            (void)stream->queue_local_end_stream();
        }
        return http2_data_submit_status::queued;
    }
    if (http2_ends_stream(end_stream)) {
        (void)stream->commit_local_end_stream();
        release_local_request_stream_if_closed(*stream);
    }
    retire_completed_local_push(stream_id);
    return http2_data_submit_status::accepted;
}

http2_request_content_release_status http2_connection::release_request_content(
    std::uint32_t stream_id) noexcept {
    auto* stream = find_stream(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return http2_request_content_release_status::closed;
    }
    return stream->release_request_continue() ? http2_request_content_release_status::released
                                              : http2_request_content_release_status::not_pending;
}

http2_submit_status http2_connection::submit_connect_response_head(
    std::uint32_t stream_id, const http_response& response) {
    auto* stream = find_stream(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return http2_submit_status::closed;
    }
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) ||
        stream->local_send().head_pending() == nullptr || stream->tunnel().pending() == nullptr) {
        return http2_submit_status::invalid_state;
    }
    if (!http2_is_valid_connect_response_head(response)) {
        return http2_submit_status::invalid_message;
    }
    const auto control_result = http2_final_response_control_plan(response);
    const auto* http2_control = control_result.control();
    if (http2_control == nullptr) {
        return http2_submit_status::invalid_message;
    }
    const auto head_plan_result = http2_connect_response_head_plan(
        plan_http_response_body(http_known_method::connect, response.status()));
    const auto* head_plan = head_plan_result.plan();
    if (head_plan == nullptr) {
        return http2_submit_status::invalid_message;
    }
    const bool terminal_remote_half = http2_remote_peer_half_closed(*stream);
    if (terminal_remote_half) {
        reserve_event_slots(1);
    }
    if (!append_http2_response_headers(*stream, response, *head_plan, *http2_control)) {
        return http2_submit_status::invalid_message;
    }
    commit_connect_response_head(*stream, terminal_remote_half);
    return http2_submit_status::accepted;
}

http2_websocket_handshake_submit_result http2_connection::submit_websocket_handshake(
    std::uint32_t stream_id, websocket_server_negotiation&& negotiation) {
    auto* stream = find_stream(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return http2_websocket_handshake_submit_result::make_failure(
            http2_websocket_handshake_submit_error::closed);
    }
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) ||
        http2_remote_peer_half_closed(*stream) || stream->local_send().head_pending() == nullptr ||
        !http2_is_pending_websocket_connect(*stream)) {
        return http2_websocket_handshake_submit_result::make_failure(
            http2_websocket_handshake_submit_error::invalid_state);
    }
    const bool terminal_remote_half = http2_remote_peer_half_closed(*stream);
    if (terminal_remote_half) {
        reserve_event_slots(1);
    }
    http2_encode_websocket_handshake_headers(stream->local_header_block(), negotiation);
    commit_connect_response_head(*stream, terminal_remote_half);
    return http2_websocket_handshake_submit_result::make_submitted(std::move(negotiation));
}

http2_websocket_handshake_submit_result http2_connection::submit_websocket_handshake(
    std::uint32_t stream_id, const websocket_handshake_validation_result& validation,
    websocket_server_negotiation&& negotiation) {
    auto* stream = find_stream(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return http2_websocket_handshake_submit_result::make_failure(
            http2_websocket_handshake_submit_error::closed);
    }
    if (role_ != http2_role::server || !http2_remote_final_head_decoded(*stream) ||
        http2_remote_peer_half_closed(*stream) || stream->local_send().head_pending() == nullptr ||
        !http2_is_pending_websocket_connect(*stream) || validation.accepted() == nullptr) {
        return http2_websocket_handshake_submit_result::make_failure(
            http2_websocket_handshake_submit_error::invalid_state);
    }
    return submit_websocket_handshake(stream_id, std::move(negotiation));
}

http2_websocket_handshake_submit_result http2_connection::submit_websocket_handshake(
    std::uint32_t stream_id, const http_request& request,
    const websocket_handshake_validation_result& validation) {
    auto negotiation = make_websocket_server_negotiation(request);
    return submit_websocket_handshake(stream_id, validation, std::move(negotiation));
}

http2_finish_request_status http2_connection::finish_request(std::uint32_t stream_id,
    std::span<const http_header_view> trailers) {
    using status_type = http2_finish_request_status;
    auto* stream = find_stream(stream_id);
    if (!stream || stream->is_aborted()) {
        return status_type::closed;
    }
    if (role_ != http2_role::client || stream->local_send().request_content_open() == nullptr ||
        stream->request_continue_pending() || stream->request_content_canceled()) {
        return status_type::invalid_state;
    }
    if (!stream->local_content().length_complete()) {
        return status_type::content_length_incomplete;
    }
    if (trailers.size() > max_http_header_fields) {
        return status_type::invalid_trailer;
    }
    http_header_section_size size;
    for (const auto& field : trailers) {
        if (!http2_is_valid_regular_header(field.name(), field.value()) ||
            is_forbidden_http_request_trailer_name(field.name()) || !size.add(field.name(), field.value())) {
            return status_type::invalid_trailer;
        }
    }
    std::pmr::string block(resource_);
    for (const auto& field : trailers) {
        hpack_encoder::encode_header(block, field.name(), field.value());
    }
    for (auto& pending : pending_sends_) {
        if (pending.stream_id_ == stream_id) {
            pending.end_stream_ = block.empty() ? http2_end_stream::end_stream : http2_end_stream::keep_open;
            pending.trailer_block_.swap(block);
            (void)stream->queue_local_end_stream();
            return status_type::queued;
        }
    }
    if (block.empty()) {
        output_.append_frame(http2_frame_type::data, http2_flag_end_stream, stream_id, {});
    } else {
        append_response_header_frames(*stream, block, http2_end_stream::end_stream);
    }
    (void)stream->commit_local_end_stream();
    release_local_request_stream_if_closed(*stream);
    return status_type::accepted;
}

http2_finish_submit_status http2_connection::finish_response(
    std::uint32_t stream_id, const http_response_trailer_section& trailers) {
    auto* stream = find_stream(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return http2_finish_submit_status::closed;
    }
    if (stream->local_send().response_content_open() == nullptr &&
        stream->local_send().response_trailers_only() == nullptr) {
        return http2_finish_submit_status::invalid_state;
    }
    if (!stream->local_content().length_complete()) {
        return http2_finish_submit_status::content_length_incomplete;
    }
    if (stream->local_send().response_trailers_only() != nullptr && trailers.empty()) {
        // A trailers-only response cannot fall back to DATA(END_STREAM): its
        // method/status explicitly forbids DATA, including an empty terminal frame.
        return http2_finish_submit_status::invalid_state;
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
            return http2_finish_submit_status::queued;
        }
    }
    if (trailer_block.empty()) {
        output_.append_frame(http2_frame_type::data, http2_flag_end_stream, stream_id, {});
        (void)stream->commit_local_end_stream();
        retire_completed_local_push(stream_id);
        return http2_finish_submit_status::accepted;
    }
    append_response_header_frames(*stream, std::string_view(trailer_block), http2_end_stream::end_stream);
    (void)stream->commit_local_end_stream();
    retire_completed_local_push(stream_id);
    return http2_finish_submit_status::accepted;
}

http2_submit_status http2_connection::submit_reset(std::uint32_t stream_id, http2_error_code error) {
    if (stream_id == 0) {
        return http2_submit_status::invalid_state;
    }
    auto* stream = find_stream(stream_id);
    if (stream == nullptr) {
        return closed_streams_.source(stream_id).has_value() ? http2_submit_status::closed
                                                             : http2_submit_status::invalid_state;
    }
    if (stream->is_aborted()) {
        return http2_submit_status::closed;
    }
    // A client-created stream is still RFC-idle until its request HEADERS are
    // submitted; RST_STREAM on that state would make the peer close the connection.
    // A server owner does not own a peer stream until its initial header block has
    // decoded; rejecting an early reset also preserves the mandatory CONTINUATION run.
    if ((role_ == http2_role::client && stream->local_send().head_pending() != nullptr) ||
        (role_ == http2_role::server && !http2_remote_final_head_decoded(*stream)) ||
        http2_stream_is_closed(*stream)) {
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
