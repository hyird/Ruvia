#include <algorithm>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>

#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/detail/response/http_response_body_access.h"
#include "ruvia/http/detail/server/http_final_response_control_plan.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/http_request_content_semantics.h"
#include "ruvia/http/http_response_stream.h"

#include "http2/http2_connection.h"
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

void http2_connection::submit_header_frames(http2_stream_state& stream, http2_end_stream end_stream) {
    const auto fields = fields_.nv();
    check_nghttp2(nghttp2_submit_headers(session_.get(),
        static_cast<std::uint8_t>(http2_ends_stream(end_stream) ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE),
        static_cast<std::int32_t>(stream.id()), nullptr, fields.data(), fields.size(), nullptr));
    fields_.release();
    if (stream.push_reservation() == http2_push_reservation::local && stream.hold_peer_concurrency_slot()) {
        ++active_local_request_streams_;
    }
    stream.activate_push();
}

void http2_connection::send_data(std::uint32_t stream_id, std::string_view bytes, http2_end_stream end_stream) {
    // The caller clamped `bytes` to both send windows, so nghttp2 serializes
    // all of it while this call drains the session; nothing is retained.
    outbound_data_type data{.bytes_ = bytes};
    nghttp2_data_provider2 provider{};
    provider.source.ptr = &data;
    provider.read_callback = &http2_connection::read_outbound_data;
    check_nghttp2(nghttp2_submit_data2(session_.get(),
        static_cast<std::uint8_t>(http2_ends_stream(end_stream) ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE),
        static_cast<std::int32_t>(stream_id), &provider));
    flush_session();
    if (!data.finished_) {
        // nghttp2 kept a pointer to this frame's source; the session cannot continue.
        fail_connection(http2_error_code::internal_error);
        flush_output();
        throw std::logic_error("nghttp2 deferred DATA within its send window");
    }
}

void http2_connection::commit_connect_response_head(
    http2_stream_state& stream, bool terminal_remote_half) {
    submit_header_frames(stream, http2_end_stream::keep_open);
    (void)stream.accept_connect();
    stream.begin_local_content_unbounded();
    (void)stream.open_local_connect_tunnel();
    if (terminal_remote_half) {
        publish_event(http2_event(http2_tunnel_end_event(stream.id())));
    }
    flush_session();
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
    // RFC 9113 §8.4: a pushed response needs a slot under the client's
    // SETTINGS_MAX_CONCURRENT_STREAMS.
    if (stream->push_reservation() == http2_push_reservation::local &&
        active_local_request_streams_ >= peer_setting(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS)) {
        return http2_response_head_submit_error::peer_stream_limit_reached;
    }
    return std::nullopt;
}

bool http2_connection::commit_final_response_head(http2_stream_state& stream, const http_response& response,
    const http_framed_response_head_plan& head_plan, http2_end_stream end_stream,
    std::optional<std::uint64_t> content_length) {
    if (!append_http2_response_headers(fields_, response, head_plan)) {
        return false;
    }
    submit_header_frames(stream, end_stream);
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
    release_local_request_stream_if_closed(stream);
    flush_session();
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
    const auto head_plan_result = http_framed_buffered_response_head_plan(write_plan, response);
    const auto* head_plan = std::get_if<http_framed_response_head_plan>(&head_plan_result);
    if (head_plan == nullptr) {
        const auto error = std::get<http_framed_response_head_plan_error>(head_plan_result);
        const bool response_plan_mismatch =
            error == http_framed_response_head_plan_error::response_status_mismatch ||
            error == http_framed_response_head_plan_error::response_representation_mismatch;
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
    // local DATA accounting contract.
    const auto head_plan_result =
        http_framed_streaming_response_head_plan(commit_plan.body_plan(), stream_head.response());
    const auto* head_plan = std::get_if<http_framed_response_head_plan>(&head_plan_result);
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
    if (append_http2_interim_response_headers(fields_, response) !=
        http2_interim_response_header_encode_status::ok) {
        return http2_submit_status::invalid_message;
    }
    submit_header_frames(*stream, http2_end_stream::keep_open);
    flush_session();
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
    const auto window = send_window_state(stream_id);
    const std::size_t available = window.has_value() ? window->available_ : 0;
    const auto consumed = (std::min)({chunk.size(), max_payload_bytes, available});
    const bool complete = consumed == chunk.size();
    const auto terminal = complete ? end_stream : http2_end_stream::keep_open;
    // An END_STREAM without payload is still a DATA frame, which nghttp2 sends
    // only with positive connection and stream windows.
    const bool empty_terminal = chunk.empty() && http2_ends_stream(terminal);
    if (empty_terminal && !can_send_empty_end_stream(stream_id)) {
        return {0, http2_data_submit_status::backpressured};
    }
    if (consumed != 0 || empty_terminal) {
        // Commit before nghttp2 sends the frame: an END_STREAM that completes the
        // stream closes it during the same drain.
        stream->commit_local_content(consumed);
        if (http2_ends_stream(terminal)) {
            (void)stream->commit_local_end_stream();
            release_local_request_stream_if_closed(*stream);
        }
        (void)send_data(stream_id, chunk.substr(0, consumed), terminal);
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
    if (!http2_is_valid_connect_response_head(response) ||
        http2_final_response_control_plan(response).has_value()) {
        return http2_submit_status::invalid_message;
    }
    const auto head_plan_result = http_framed_connect_response_head_plan(
        plan_http_response_body(http_known_method::connect, response.status()));
    const auto* head_plan = std::get_if<http_framed_response_head_plan>(&head_plan_result);
    if (head_plan == nullptr || !append_http2_response_headers(fields_, response, *head_plan)) {
        return http2_submit_status::invalid_message;
    }
    commit_connect_response_head(*stream, http2_remote_peer_half_closed(*stream));
    return http2_submit_status::accepted;
}

std::variant<websocket_extended_connect_negotiation, http2_websocket_handshake_submit_error>
http2_connection::submit_websocket_handshake(
    std::uint32_t stream_id, const http_request& request,
    const websocket_handshake_validation_result& validation,
    websocket_server_handshake_options options) {
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
    auto negotiation = make_websocket_server_negotiation(request, options, resource_);
    http2_append_websocket_handshake_headers(fields_, negotiation);
    commit_connect_response_head(*stream, http2_remote_peer_half_closed(*stream));
    const auto compression = negotiation.compression();
    return websocket_extended_connect_negotiation(std::move(negotiation).take_subprotocol(), compression);
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
        if (!can_send_empty_end_stream(stream_id)) {
            return http2_finish_status::backpressured;
        }
    } else {
        http2_header_list_size size;
        for (const auto& field : trailers) {
            if (!http2_is_valid_outbound_regular_header(field.name(), field.value()) ||
                is_forbidden_http_request_trailer_name(field.name()) || !size.add(field.name(), field.value())) {
                throw std::invalid_argument("invalid HTTP/2 request trailer");
            }
        }
        fields_.clear();
        fields_.reserve(trailers.size());
        fields_.add_lowercase_fields(trailers);
    }
    end_local_stream(*stream, trailers.empty());
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
    if (trailers.empty()) {
        if (!can_send_empty_end_stream(stream_id)) {
            return http2_finish_status::backpressured;
        }
    } else {
        append_http2_response_trailers(fields_, trailers);
    }
    end_local_stream(*stream, trailers.empty());
    return http2_finish_status::accepted;
}

void http2_connection::end_local_stream(http2_stream_state& stream, bool empty_data) {
    const auto stream_id = stream.id();
    if (empty_data) {
        // Commit before nghttp2 sends the frame: an END_STREAM that completes the
        // stream closes it during the same drain.
        (void)stream.commit_local_end_stream();
        release_local_request_stream_if_closed(stream);
        send_data(stream_id, {}, http2_end_stream::end_stream);
        return;
    }
    submit_header_frames(stream, http2_end_stream::end_stream);
    (void)stream.commit_local_end_stream();
    release_local_request_stream_if_closed(stream);
    flush_session();
}

http2_submit_status http2_connection::submit_reset(std::uint32_t stream_id, http2_error_code error) {
    if (stream_id == 0 || stream_id > http2_max_stream_id) {
        return http2_submit_status::invalid_state;
    }
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr) {
        // nghttp2 keeps no closed-stream history (RFC 9113 §5.1 lets an endpoint
        // forget closed streams); a known identifier is simply closed.
        const bool local_id = ((stream_id & 1U) != 0) == (role_ == http2_role::client);
        const bool used = local_id ? stream_id < nghttp2_session_get_next_stream_id(session_.get())
                                   : stream_id <= highest_peer_stream_id_;
        return used ? http2_submit_status::closed : http2_submit_status::invalid_state;
    }
    if (stream->is_aborted()) {
        return http2_submit_status::closed;
    }
    if (local_connection_state_.fatal_failure() != nullptr) {
        // Owner release still retires the stream and its leases after failure;
        // only the wire reset is suppressed on the terminal connection.
        return close_stream_impl(stream_id, http2_stream_close_source::local, http2_error_code::no_error,
                   close_notification_type::owner_already_knows)
                   ? http2_submit_status::accepted
                   : http2_submit_status::closed;
    }
    // A server owner does not own a peer stream until its initial header block
    // has decoded. A stream whose halves both ended is closed on the wire (RFC
    // 9113 §5.1): only a PROTOCOL_ERROR reset for a malformed message is
    // accepted, and it retires the stream locally without a frame.
    if ((role_ == http2_role::server && !http2_remote_final_head_decoded(*stream)) ||
        (http2_stream_is_closed(*stream) && error != http2_error_code::protocol_error)) {
        return http2_submit_status::invalid_state;
    }
    if (!http2_stream_is_closed(*stream)) {
        submit_owner_reset(stream_id, error);
    }
    cancel_push_promise_on(stream_id);
    (void)close_stream_impl(stream_id, http2_stream_close_source::local, error,
        close_notification_type::owner_already_knows);
    flush_session();
    return http2_submit_status::accepted;
}

}  // namespace ruvia::detail
