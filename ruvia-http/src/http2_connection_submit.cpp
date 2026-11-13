#include <nghttp2/nghttp2.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "ruvia/http/http2_connection.h"
#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_client.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/http_status.h"
#include "ruvia/http/url_encoding.h"
#include "ruvia/http/websocket_handshake.h"

#include "http2_connection.h"
#include "http_connection_fields.h"
#include "http_websocket_permessage_deflate.h"
#include "http_zlib_stream.h"

namespace ruvia::detail {
namespace {

// nghttp2 packs an extension payload into a buffer of at least 16 KiB
// (nghttp2_pack_extension_callback2), so larger peer frame limits do not apply.
[[nodiscard]] std::uint32_t extension_payload_limit(std::uint32_t peer_max_frame_size) noexcept {
    return (std::min)(peer_max_frame_size, http2_default_max_frame_size);
}

}  // namespace

void http2_connection::apply_response_priority(http2_stream_state& stream, const http_response_headers& headers) {
    const auto parsed = parse_http_priority(headers);
    const auto* fields = std::get_if<http_priority_fields>(&parsed);
    if (fields == nullptr || (!fields->urgency_ && !fields->incremental_)) {
        return;
    }
    nghttp2_extpri priority{NGHTTP2_EXTPRI_DEFAULT_URGENCY, 0};
    if (nghttp2_session_get_extpri_stream_priority(
            session_.get(), &priority, static_cast<std::int32_t>(stream.id())) != 0) {
        return;
    }
    // Response parameters override only their specified request parameters.
    if (fields->urgency_) {
        priority.urgency = fields->urgency_->value();
    }
    if (fields->incremental_) {
        priority.inc = *fields->incremental_ ? 1 : 0;
    }
    check_nghttp2(nghttp2_session_change_extpri_stream_priority(
        session_.get(), static_cast<std::int32_t>(stream.id()), &priority, 0));
}

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
    if (!append_http_binary_response_fields(fields_, response, head_plan, max_http_header_bytes)) {
        return false;
    }
    apply_response_priority(stream, response.headers());
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
    if (http_binary_final_response_control_plan(response).has_value()) {
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
    if (http_binary_final_response_control_plan(head).has_value()) {
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
    if (!append_http_binary_interim_response_fields(fields_, response, max_http_header_bytes)) {
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
        case http_local_content_check::accepted:
            break;
        case http_local_content_check::not_started:
        case http_local_content_check::forbidden:
            throw std::logic_error("HTTP/2 stream content is not allowed");
        case http_local_content_check::length_exceeded:
        case http_local_content_check::length_incomplete:
            return {0, http2_data_submit_status::invalid_message};
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
    if (!is_valid_http_binary_connect_response_head(response) ||
        http_binary_final_response_control_plan(response).has_value()) {
        return http2_submit_status::invalid_message;
    }
    const auto head_plan_result = http_framed_connect_response_head_plan(
        plan_http_response_body(http_known_method::connect, response.status()));
    const auto* head_plan = std::get_if<http_framed_response_head_plan>(&head_plan_result);
    if (head_plan == nullptr || !append_http_binary_response_fields(fields_, response, *head_plan, max_http_header_bytes)) {
        return http2_submit_status::invalid_message;
    }
    apply_response_priority(*stream, response.headers());
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
    auto negotiation = make_websocket_server_negotiation(request, options);
    if (!append_http_binary_websocket_response_fields(fields_, negotiation, max_http_header_bytes)) {
        throw std::invalid_argument("WebSocket response headers exceed HTTP/2 header list limit");
    }
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
        return http2_finish_status::invalid_message;
    }
    if (trailers.empty()) {
        if (!can_send_empty_end_stream(stream_id)) {
            return http2_finish_status::backpressured;
        }
    } else {
        binary_field_section_size size(max_http_header_bytes);
        for (const auto& field : trailers) {
            if (!http_binary_is_valid_outbound_regular_header(field.name(), field.value()) ||
                is_forbidden_http_request_trailer_name(field.name()) || !size.add(field.name(), field.value())) {
                return http2_finish_status::invalid_message;
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
        return http2_finish_status::invalid_message;
    }
    if (trailers.empty()) {
        if (!can_send_empty_end_stream(stream_id)) {
            return http2_finish_status::backpressured;
        }
    } else {
        fields_.clear();
        fields_.reserve(trailers.fields().size());
        fields_.add_lowercase_fields(trailers.fields());
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

http2_submit_status http2_connection::submit_extension_frame(
    http2_frame_type type, std::uint32_t stream_id, http2_extension_payload& payload) {
    check_nghttp2(nghttp2_submit_extension(session_.get(), static_cast<std::uint8_t>(type), NGHTTP2_FLAG_NONE,
        static_cast<std::int32_t>(stream_id), &payload));
    flush_session();
    return http2_submit_status::accepted;
}

http2_submit_status http2_connection::submit_origin_advertisement(std::span<const std::string_view> origins) {
    if (role_ != http2_role::server) {
        return http2_submit_status::invalid_state;
    }
    if (local_connection_state_.fatal_failure() != nullptr || peer_goaway_) {
        return http2_submit_status::closed;
    }
    if (!received_peer_settings_) {
        return http2_submit_status::invalid_state;
    }
    const auto payload_size = http_origin_payload_size(
        origins, extension_payload_limit(peer_setting(NGHTTP2_SETTINGS_MAX_FRAME_SIZE)));
    if (payload_size.index() != 0) {
        return http2_submit_status::invalid_message;
    }
    http2_extension_payload payload{
        .size_ = std::get<0>(payload_size),
        .write_ = [](const http2_extension_payload& value, std::span<char> output) noexcept {
            write_http_origin_payload(output, value.origins_);
        },
        .origins_ = origins,
    };
    return submit_extension_frame(http2_frame_type::origin, 0, payload);
}

http2_submit_status http2_connection::submit_alternative_service_advertisement(std::uint32_t stream_id,
    std::string_view origin, std::string_view field_value) {
    if (role_ != http2_role::server) {
        return http2_submit_status::invalid_state;
    }
    if (local_connection_state_.fatal_failure() != nullptr || peer_goaway_) {
        return http2_submit_status::closed;
    }
    const auto* stream = stream_id != 0 ? streams_.find(stream_id) : nullptr;
    if (!received_peer_settings_ ||
        (stream_id != 0 &&
            (stream == nullptr || http2_stream_is_closed(*stream) ||
                stream->push_reservation() == http2_push_reservation::local ||
                !http2_remote_final_head_decoded(*stream) || stream->tunnel().not_connect() == nullptr))) {
        return http2_submit_status::invalid_state;
    }
    const auto payload_size = http2_alternative_service_payload_size(
        stream_id, origin, field_value, extension_payload_limit(peer_setting(NGHTTP2_SETTINGS_MAX_FRAME_SIZE)));
    if (payload_size.index() != 0) {
        return http2_submit_status::invalid_message;
    }
    http2_extension_payload payload{
        .size_ = std::get<0>(payload_size),
        .write_ = [](const http2_extension_payload& value, std::span<char> output) noexcept {
            http2_write_alternative_service_payload(output, value.origin_, value.field_value_);
        },
        .origin_ = origin,
        .field_value_ = field_value,
    };
    return submit_extension_frame(http2_frame_type::alternative_service, stream_id, payload);
}
}  // namespace ruvia::detail

namespace ruvia {

namespace {
using error_type = http_connection_advertisement_error;
constexpr std::size_t length_prefix_bytes = 2;
std::size_t length(std::span<const char> bytes_value) {
    return (static_cast<unsigned char>(bytes_value[0]) << 8) | static_cast<unsigned char>(bytes_value[1]);
}
// RFC 7838 §4: ALTSVC on stream 0 carries an origin; on a stream it does not.
bool is_valid_alternative_service_stream(std::uint32_t stream_id, bool has_origin) {
    return stream_id <= detail::http2_max_stream_id && (stream_id == 0) == has_origin;
}
std::vector<char> make_http2_frame(http2_frame_type type, std::uint32_t stream_id, std::size_t payload_size) {
    std::vector<char> bytes(http2_frame_header_bytes + payload_size);
    (void)encode_http2_frame_header(
        std::span(bytes).first(http2_frame_header_bytes), static_cast<std::uint32_t>(payload_size), type, 0, stream_id);
    return bytes;
}
}  // namespace
std::variant<http_origin_advertisement, error_type> decode_http_origin_advertisement(
    std::span<const char> payload_value) {
    http_origin_advertisement result;
    while (!payload_value.empty()) {
        if (payload_value.size() < length_prefix_bytes) {
            return error_type::malformed_payload;
        }
        const auto size = length(payload_value);
        payload_value = payload_value.subspan(length_prefix_bytes);
        if (size > payload_value.size()) {
            return error_type::malformed_payload;
        }
        const std::string_view origin(payload_value.data(), size);
        if (is_valid_http_serialized_origin(origin)) {
            detail::reserve_geometric(result.origins_, result.origins_.size() + 1);
            result.origins_.emplace_back(origin);
        }
        payload_value = payload_value.subspan(size);
    }
    return result;
}
std::variant<std::vector<char>, error_type> encode_http2_origin_frame(std::span<const std::string_view> origins,
    std::uint32_t maximum) {
    const auto payload_size =
        detail::http_origin_payload_size(origins, std::min(maximum, http2_max_frame_size));
    if (payload_size.index() != 0) {
        return std::get<1>(payload_size);
    }
    auto bytes_value = make_http2_frame(http2_frame_type::origin, 0, std::get<0>(payload_size));
    detail::write_http_origin_payload(std::span(bytes_value).subspan(http2_frame_header_bytes), origins);
    return bytes_value;
}
std::variant<http_alternative_service_advertisement, error_type> decode_http2_alternative_service(std::uint32_t stream_id,
    std::span<const char> payload_value) {
    if (payload_value.size() < length_prefix_bytes) {
        return error_type::malformed_payload;
    }
    const auto size = length(payload_value);
    payload_value = payload_value.subspan(length_prefix_bytes);
    if (size > payload_value.size()) {
        return error_type::malformed_payload;
    }
    if (!is_valid_alternative_service_stream(stream_id, size != 0)) {
        return error_type::invalid_stream;
    }
    const std::string_view origin(payload_value.data(), size);
    if (!origin.empty() && !is_valid_http_serialized_origin(origin)) {
        return error_type::invalid_origin;
    }
    const std::string_view value(payload_value.data() + size, payload_value.size() - size);
    if (!is_valid_http_header_value(value)) {
        return error_type::invalid_field;
    }
    http_alternative_service_advertisement result;
    result.stream_id_ = stream_id;
    result.origin_ = origin;
    result.field_value_ = value;
    return result;
}
std::variant<std::vector<char>, error_type> encode_http2_alternative_service_frame(std::uint32_t stream_id,
    std::string_view origin, std::string_view value, std::uint32_t maximum) {
    const auto payload_size = detail::http2_alternative_service_payload_size(stream_id, origin, value, maximum);
    if (payload_size.index() != 0) {
        return std::get<1>(payload_size);
    }
    auto bytes_value =
        make_http2_frame(http2_frame_type::alternative_service, stream_id, std::get<0>(payload_size));
    detail::http2_write_alternative_service_payload(
        std::span(bytes_value).subspan(http2_frame_header_bytes), origin, value);
    return bytes_value;
}
namespace detail {
std::variant<std::size_t, error_type> http2_alternative_service_payload_size(
    std::uint32_t stream_id, std::string_view origin, std::string_view field_value, std::uint32_t max_frame_size) {
    if (!is_valid_alternative_service_stream(stream_id, !origin.empty())) {
        return error_type::invalid_stream;
    }
    if (!origin.empty() && !is_valid_http_serialized_origin(origin)) {
        return error_type::invalid_origin;
    }
    if (!is_valid_http_header_value(field_value)) {
        return error_type::invalid_field;
    }
    std::size_t remaining = std::min(max_frame_size, http2_max_frame_size);
    if (!reserve_http_prefixed_origin(remaining, origin) || field_value.size() > remaining) {
        return error_type::limit;
    }
    return length_prefix_bytes + origin.size() + field_value.size();
}
void http2_write_alternative_service_payload(
    std::span<char> payload, std::string_view origin, std::string_view field_value) noexcept {
    auto* out = write_http_prefixed_origin(payload.data(), origin);
    if (!field_value.empty()) {
        std::memcpy(out, field_value.data(), field_value.size());
    }
}
}  // namespace detail
}  // namespace ruvia

// Submitting a request head as the client: what an outbound :method / :path /
// :authority and its header section must satisfy before the connection will
// encode it, for a regular request, a CONNECT tunnel, and the extended CONNECT
// that carries a websocket handshake.

namespace ruvia::detail {

template <typename prepare_type>
std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_local_request_head(
    http2_end_stream end_stream, const prepare_type& prepare) {
    if (active_local_request_streams_ >= http2_local_settings::max_concurrent_streams) {
        return http2_request_head_submit_error::local_stream_capacity_reached;
    }
    // nghttp2 assigns this identifier to the next request HEADERS.
    const auto stream_id = static_cast<std::uint32_t>(nghttp2_session_get_next_stream_id(session_.get()));
    auto* stream = create_stream(stream_id);
    if (stream == nullptr) {
        return http2_request_head_submit_error::local_stream_capacity_reached;
    }
    std::int32_t submitted = 0;
    try {
        prepare(*stream);
        const auto fields = fields_.nv();
        submitted = nghttp2_submit_headers(session_.get(),
            static_cast<std::uint8_t>(http2_ends_stream(end_stream) ? NGHTTP2_FLAG_END_STREAM : NGHTTP2_FLAG_NONE), -1,
            nullptr, fields.data(),
            fields.size(), nullptr);
        check_nghttp2(submitted);
    } catch (...) {
        fields_.clear();
        (void)streams_.remove(stream_id);
        throw;
    }
    fields_.release();
    if (static_cast<std::uint32_t>(submitted) != stream_id) {
        std::terminate();
    }
    if (stream->hold_peer_concurrency_slot()) {
        ++active_local_request_streams_;
    }
    flush_session();
    return stream_id;
}

std::variant<std::uint32_t, http2_request_head_submit_error>
http2_connection::submit_regular_request_head(std::string_view method,
    std::string_view scheme, std::optional<std::string_view> authority, std::string_view path,
    std::span<const http_header_view> headers, http_request_content_plan content,
    http_client_request_expectation expectation, http_request_field_source source) {
    if (const auto error = local_request_admission_error()) {
        return *error;
    }
    const bool without_content = content.without_content() != nullptr;
    const auto* known_length_content = content.known_length_content();
    const bool streaming_content = content.streaming_content() != nullptr;
    const auto declared_length = content.declared_length();
    const auto expected_length = declared_length ? std::optional{declared_length->value()} : std::nullopt;
    const bool content_will_follow = !without_content && (!expected_length || *expected_length != 0);
    const auto content_indication = content_will_follow ? http_request_content_indication::will_follow
                                                        : http_request_content_indication::no_content;
    auto end_stream = http2_end_stream::keep_open;
    // Only the formatted prefix is exposed; an omitted length keeps an empty view.
    std::array<char, http_max_unsigned_decimal_size> length_buffer;
    if (without_content) {
        end_stream = http2_end_stream::end_stream;
    } else if (known_length_content != nullptr) {
        end_stream = known_length_content->length().value() == 0 ? http2_end_stream::end_stream
                                                                 : http2_end_stream::keep_open;
    }
    const auto known_method = classify_http_method(method);
    const auto default_port = http_uri_scheme_default_port(scheme);
    const bool emit_length = expected_length && method != "TRACE";
    const auto content_length_value = emit_length
                                          ? http_format_unsigned(length_buffer, *expected_length)
                                          : std::string_view{};
    // Validate the entire semantic head and decoded-size budget before touching
    // the session, stream metadata, or lifecycle state.
    if (!http_binary_is_valid_outbound_regular_request_head(method, known_method, scheme, default_port,
            authority, path, headers, content_indication, content_length_value, expectation, source, max_http_header_bytes)) {
        return http2_request_head_submit_error::invalid_message;
    }
    return submit_local_request_head(end_stream, [&](http2_stream_state& stream) {
        stream.assign_request_method(method);
        stream.assign_request_scheme(scheme);
        fields_.clear();
        fields_.reserve(headers.size() + 6);
        fields_.add(":method", method);
        fields_.add(":scheme", scheme);
        if (authority.has_value()) {
            fields_.add(":authority", *authority);
        }
        fields_.add(":path", path);
        fields_.add_lowercase_fields(headers);
        if (expectation == http_client_request_expectation::continue_value) {
            fields_.add("expect", http_continue_expectation_token);
        }
        if (emit_length) {
            fields_.add_owned_value("content-length", content_length_value);
        }
        if (without_content) {
            stream.begin_local_content_forbidden();
        } else if (expected_length) {
            stream.begin_local_content_known_length(*expected_length);
        } else if (streaming_content) {
            stream.begin_local_content_unbounded();
        }
        if (http2_ends_stream(end_stream)) {
            (void)stream.commit_local_head_end_stream();
        } else {
            (void)stream.begin_local_request_content();
            if (expectation == http_client_request_expectation::continue_value) {
                stream.await_request_continue();
            }
        }
    });
}

std::variant<std::uint32_t, http2_request_head_submit_error>
http2_connection::submit_connect_request_head(
    std::string_view authority, std::span<const http_header_view> headers, http_request_field_source source) {
    if (const auto error = local_request_admission_error()) {
        return *error;
    }

    request_target_view target;
    binary_field_section_size section_size(max_http_header_bytes);
    request_field_state fields{request_field_framing::binary};
    if (!parse_request_target(http_known_method::connect, authority, target) ||
        !section_size.add(":method", "CONNECT") || !section_size.add(":authority", authority) ||
        !http_binary_are_valid_outbound_request_headers(authority, 0, headers,
            /*allow_host=*/false,
            /*allow_trailers=*/false, section_size, fields, source)) {
        return http2_request_head_submit_error::invalid_message;
    }

    return submit_local_request_head(http2_end_stream::keep_open, [&](http2_stream_state& stream) {
        (void)stream.begin_standard_connect();
        stream.assign_request_method("CONNECT");
        stream.begin_local_content_forbidden();
        (void)stream.begin_local_connect_request();
        fields_.clear();
        fields_.reserve(headers.size() + 2);
        fields_.add(":method", "CONNECT");
        fields_.add(":authority", authority);
        fields_.add_lowercase_fields(headers);
    });
}

std::variant<std::uint32_t, http2_request_head_submit_error>
http2_connection::submit_extended_connect_request_head(
    std::string_view protocol, std::string_view scheme, std::string_view authority,
    std::string_view path, std::span<const http_header_view> headers, http_request_field_source source) {
    if (const auto error = local_request_admission_error()) {
        return *error;
    }
    if (peer_setting(NGHTTP2_SETTINGS_ENABLE_CONNECT_PROTOCOL) != 1) {
        return http2_request_head_submit_error::peer_capability_unavailable;
    }

    const bool websocket_value = http_ascii_equals_ignore_case(protocol, "websocket");
    const auto default_port = http_uri_scheme_default_port(scheme);
    const auto encoded_protocol = websocket_value ? std::string_view("websocket") : protocol;
    binary_field_section_size section_size(max_http_header_bytes);
    request_field_state fields{request_field_framing::binary};
    if (!is_valid_http_header_name(protocol) || !is_valid_uri_scheme(scheme) ||
        (websocket_value && default_port == 0) || !is_valid_binary_request_authority(scheme, authority) ||
        !http_binary_is_valid_extended_connect_path(scheme, path) || !section_size.add(":method", "CONNECT") ||
        !section_size.add(":protocol", encoded_protocol) || !section_size.add(":scheme", scheme) ||
        !section_size.add(":authority", authority) || !section_size.add(":path", path) ||
        !http_binary_are_valid_outbound_request_headers(authority, default_port, headers,
            /*allow_host=*/!websocket_value,
            /*allow_trailers=*/false, section_size, fields, source,
            /*websocket_connect=*/websocket_value)) {
        return http2_request_head_submit_error::invalid_message;
    }

    return submit_local_request_head(http2_end_stream::keep_open, [&](http2_stream_state& stream) {
        (void)stream.begin_extended_connect();
        stream.assign_request_method("CONNECT");
        stream.assign_request_scheme(scheme);
        stream.set_protocol(encoded_protocol);
        stream.begin_local_content_forbidden();
        (void)stream.begin_local_connect_request();
        fields_.clear();
        fields_.reserve(headers.size() + 5);
        fields_.add(":method", "CONNECT");
        // RFC 8441 registers and requires the lowercase `websocket` value. HTTP
        // protocol-name matching is case-insensitive, so accept caller spelling but
        // never put a non-canonical websocket token on the wire or in stream state.
        fields_.add(":protocol", encoded_protocol);
        fields_.add(":scheme", scheme);
        fields_.add(":authority", authority);
        fields_.add(":path", path);
        fields_.add_lowercase_fields(headers);
    });
}

std::variant<std::uint32_t, http2_push_submit_error> http2_connection::submit_push_promise(
    std::uint32_t associated_stream_id, http_push_request_view request) {
    using error_type = http2_push_submit_error;
    const auto* parent_value = streams_.find(associated_stream_id);
    const auto promised_id = static_cast<std::uint32_t>(nghttp2_session_get_next_stream_id(session_.get()));
    if (role_ != http2_role::server || !received_peer_settings_ ||
        local_connection_state_.open() == nullptr || peer_goaway_ || !parent_value || parent_value->is_aborted() ||
        (associated_stream_id & 1U) == 0 || !http2_remote_final_head_decoded(*parent_value) ||
        parent_value->tunnel().not_connect() == nullptr || parent_value->local_send().end_stream_committed() ||
        promised_id > http2_max_stream_id) {
        return error_type::invalid_state;
    }
    if (peer_setting(NGHTTP2_SETTINGS_ENABLE_PUSH) == 0) {
        return error_type::push_disabled;
    }
    // RFC 9113 §8.4: the client's SETTINGS_MAX_CONCURRENT_STREAMS bounds pushed
    // responses. When no pushed stream could open now the promise is not made.
    // Reserved promises do not count (§5.1.2); their response head waits for a slot.
    if (active_local_request_streams_ >= peer_setting(NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS)) {
        return error_type::stream_limit;
    }
    const auto known_method = request.method_ == "GET"    ? http_known_method::get
                              : request.method_ == "HEAD" ? http_known_method::head
                                                          : http_known_method::unknown;
    const auto default_port = http_uri_scheme_default_port(request.scheme_);
    if (known_method == http_known_method::unknown ||
        !http_binary_is_valid_outbound_regular_request_head(request.method_.view(), known_method,
            request.scheme_.view(), default_port, request.authority_.view(), request.path_.view(),
            request.headers_,
            http_request_content_indication::no_content, {}, http_client_request_expectation::none,
            http_request_field_source::generated, max_http_header_bytes)) {
        return error_type::invalid_request;
    }
    auto* stream = create_stream(promised_id);
    if (stream == nullptr) {
        return error_type::stream_limit;
    }
    try {
        stream->assign_request_method(request.method_);
        stream->assign_request_scheme(request.scheme_);
        stream->assign_request_authority(request.authority_);
        stream->assign_request_path(request.path_);
        stream->mark_scheme(default_port);
        stream->mark_authority();
        stream->mark_path();
        std::array<char, 64> lower_name_stack;
        std::string lower_name_scratch;
        for (std::size_t i = 0; i < request.headers_.size(); ++i) {
            const auto& header = request.headers_[i];
            const auto kind = classify_http_field(header.name());
            if (kind == http_field_kind::host) {
                stream->mark_host(stream->remote_header_count());
            }
            if (kind == http_field_kind::cookie) {
                if (!http2_append_cookie_header_value(*stream, header.value())) {
                    throw std::length_error("HTTP/2 push request cookie is too large");
                }
            } else {
                const auto lower_name = http_lower_header_name(header.name(), [&](std::size_t size) {
                    if (size <= lower_name_stack.size()) {
                        return lower_name_stack.data();
                    }
                    lower_name_scratch.resize(size);
                    return lower_name_scratch.data();
                });
                if (!stream->append_remote_header(lower_name, header.value(), kind)) {
                    throw std::length_error("HTTP/2 push request has too many fields");
                }
            }
        }
        (void)stream->record_remote_head_end_stream();
        (void)stream->finalize_remote_content_head();
        stream->reserve_push(http2_push_reservation::local);
        fields_.clear();
        fields_.reserve(request.headers_.size() + 4);
        fields_.add(":method", request.method_.view());
        fields_.add(":scheme", request.scheme_.view());
        fields_.add(":authority", request.authority_.view());
        fields_.add(":path", request.path_.view());
        fields_.add_lowercase_fields(request.headers_);
        const auto fields = fields_.nv();
        const auto submitted = nghttp2_submit_push_promise(session_.get(), NGHTTP2_FLAG_NONE,
            static_cast<std::int32_t>(associated_stream_id), fields.data(), fields.size(), nullptr);
        check_nghttp2(submitted);
        if (static_cast<std::uint32_t>(submitted) != promised_id) {
            std::terminate();
        }
    } catch (...) {
        fields_.clear();
        streams_.remove(promised_id);
        throw;
    }
    fields_.release();
    flush_session();
    return promised_id;
}

}  // namespace ruvia::detail

// Inbound HEADERS, PUSH_PROMISE and DATA as nghttp2 delivers them: one field
// block per on_begin_headers/on_header/on_frame_recv sequence, DATA payload in
// chunks borrowed from the fed input. nghttp2 has already enforced framing,
// stream states and flow control; this layer applies HTTP message semantics.

namespace ruvia::detail {

namespace {

// CONNECT has no request content; DATA after its HEADERS is tunnel data (RFC
// 9113 §8.5). RFC 9110 §8.6 makes Content-Length on CONNECT only a sender SHOULD
// NOT, so a declared zero is harmless, while a non-zero length would claim
// content that the stream never frames and stays malformed (as in HTTP/1).
[[nodiscard]] bool http2_connect_content_length_acceptable(const http2_stream_state& stream) noexcept {
    const auto* known = stream.remote_content().allowed_known_length();
    return known == nullptr || known->declared_length() == 0;
}

// Decoded HTTP/2 field names are already required to be lowercase. TE is
// permitted only in requests (RFC 9113 Section 8.2.2); a response carrying TE
// is malformed even when its value is exactly "trailers".
[[nodiscard]] bool http2_is_valid_decoded_response_header(
    std::string_view name, std::string_view value) noexcept {
    return http2_is_valid_regular_header(name, value) && name != "te";
}

// ':status' is first and unique; interim metadata is retained separately
// without applying sender prohibitions.
[[nodiscard]] bool accept_response_header(http2_header_decode_context& context,
    std::vector<http_header>& informational_fields, std::optional<http_received_status>& status_value,
    http_response_content_semantics& content_semantics, bool& saw_regular, std::string_view name,
    std::string_view value) {
    if (!http2_accumulate_header_list_bytes(context, name, value)) {
        return false;
    }
    auto& stream = context.stream_;
    if (name.empty()) {
        return false;
    }
    if (name.front() == ':') {
        if (name != ":status" || status_value || saw_regular) {
            return false;
        }
        // RFC 9110 §15: a received 000..099 or 600..999 is processed as a 5xx response.
        const auto status = http_received_status::try_parse(value);
        if (!status || status->status() == http_status::switching_protocols) {
            return false;
        }
        status_value = *status;
        content_semantics = classify_http_response_content_semantics(stream.request_known_method(), status->status());
        return true;
    }
    if (!status_value || !http2_is_valid_decoded_response_header(name, value)) {
        return false;
    }
    saw_regular = true;
    if (status_value->status().is_informational()) {
        // These fields cannot frame a 1xx or carry over to the final response.
        informational_fields.emplace_back(name, value);
        return true;
    }
    const auto kind = classify_http_field(name);
    // RFC 9110 §9.3.6: successful CONNECT ignores Content-Length semantics,
    // including invalid numerals; the field is retained for extension validators.
    if (kind == http_field_kind::content_length &&
        content_semantics != http_response_content_semantics::connect_tunnel) {
        http_content_length_state content_length;
        if (content_length.parse_field(value) != http_content_length_parse_status::ok ||
            !stream.declare_remote_content_length(*content_length.value())) {
            return false;
        }
    }
    return stream.append_remote_header(name, value, kind);
}

[[nodiscard]] bool http2_on_decoded_response_trailer(
    http2_header_decode_context& context_value, std::string_view name, std::string_view value) {
    if (!http2_accumulate_header_list_bytes(context_value, name, value)) {
        return false;
    }
    // Pseudo-header, connection-specific and TE fields are malformed (RFC 9113
    // §8.1, §8.2.2). A field the response trailer table does not permit binds
    // only the sender (RFC 9110 §6.5.1) and is discarded.
    if (!http2_is_valid_decoded_response_header(name, value)) {
        return false;
    }
    return is_forbidden_response_trailer_name(name) || context_value.stream_.append_remote_trailer(name, value);
}

}  // namespace

// --- HEADERS / PUSH_PROMISE ---------------------------------------------------

int http2_connection::on_begin_headers(const nghttp2_frame& frame) {
    header_block_.reset();
    if (local_connection_state_.fatal_failure() != nullptr) {
        return 0;
    }
    if (header_observer_ != nullptr) {
        observed_header_sequence_ = observed_frame_.observation_sequence_;
        observed_header_stream_ = static_cast<std::uint32_t>(frame.hd.stream_id);
        observed_field_index_ = 0;
        observe_header(http2_header_observation_phase::begin);
    }
    if (frame.hd.type == NGHTTP2_HEADERS && receive_frame_metadata_ &&
        (frame.hd.flags & NGHTTP2_FLAG_PRIORITY) != 0) {
        // RFC 9113 §5.3.2 priority fields are advisory metadata, queued ahead
        // of every event the frame produces.
        publish_event(http2_event(http2_received_priority{
            .stream_id_ = static_cast<std::uint32_t>(frame.hd.stream_id),
            .stream_dependency_ = static_cast<std::uint32_t>(frame.headers.pri_spec.stream_id),
            .weight_ = static_cast<std::uint16_t>(frame.headers.pri_spec.weight),
            .exclusive_ = frame.headers.pri_spec.exclusive != 0}));
    }
    if (role_ == http2_role::server) {
        return begin_request_headers(frame);
    }
    return frame.hd.type == NGHTTP2_PUSH_PROMISE ? begin_push_promise(frame) : begin_response_headers(frame);
}

int http2_connection::begin_request_headers(const nghttp2_frame& frame) {
    const auto stream_id = static_cast<std::uint32_t>(frame.hd.stream_id);
    const bool end_stream = (frame.hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
    if (frame.headers.cat == NGHTTP2_HCAT_REQUEST) {
        const auto* drain = local_connection_state_.graceful_drain();
        const bool beyond_round_trip =
            drain != nullptr && drain->awaiting_round_trip() && stream_id > drain->round_trip_stream_limit();
        auto* stream = beyond_round_trip || (drain != nullptr && stream_id > drain->last_stream_id())
                           ? nullptr
                           : create_stream(stream_id);
        if (stream == nullptr) {
            // RFC 9113 §8.7: refused before any processing; the client may
            // retry. The refusal is queued ahead of a final GOAWAY because
            // sending that GOAWAY closes every stream above its boundary.
            check_nghttp2(nghttp2_submit_rst_stream(
                session_.get(), NGHTTP2_FLAG_NONE, frame.hd.stream_id, NGHTTP2_REFUSED_STREAM));
            if (beyond_round_trip) {
                // The final boundary is the high-water mark before this stream.
                finish_drain_round_trip();
            }
            return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
        }
        last_peer_stream_id_ = stream_id;
        highest_peer_stream_id_ = (std::max)(highest_peer_stream_id_, stream_id);
        if (end_stream) {
            (void)stream->record_remote_head_end_stream();
        }
        header_block_.emplace(header_block_kind::request, *stream);
        return 0;
    }

    auto* stream = streams_.find(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        // A locally reset stream: nghttp2 still decodes the block for HPACK.
        return 0;
    }
    // CONNECT has no trailers, and a second initial head or a trailer section
    // without END_STREAM is malformed (RFC 9113 §8.1, §8.5).
    if (stream->tunnel().open() != nullptr || stream->tunnel().pending() != nullptr ||
        stream->remote_receive().content_open() == nullptr || !end_stream) {
        reset_stream_for_peer_error(stream_id, http2_error_code::protocol_error);
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    header_block_.emplace(header_block_kind::request_trailers, *stream);
    return 0;
}

int http2_connection::begin_response_headers(const nghttp2_frame& frame) {
    const auto stream_id = static_cast<std::uint32_t>(frame.hd.stream_id);
    const bool end_stream = (frame.hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
    auto* stream = streams_.find(stream_id);
    if (stream == nullptr || stream->is_aborted()) {
        return 0;
    }
    if (!http2_remote_final_head_decoded(*stream)) {
        // nghttp2 already enforced SETTINGS_MAX_CONCURRENT_STREAMS for a pushed
        // response opening its stream (RFC 9113 §5.1.2).
        if (end_stream) {
            (void)stream->record_remote_head_end_stream();
        }
        header_block_.emplace(header_block_kind::response, *stream);
        return 0;
    }
    if (stream->tunnel().open() != nullptr || stream->remote_receive().content_open() == nullptr || !end_stream) {
        reset_stream_for_peer_error(stream_id, http2_error_code::protocol_error);
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    header_block_.emplace(header_block_kind::response_trailers, *stream);
    return 0;
}

int http2_connection::begin_push_promise(const nghttp2_frame& frame) {
    // RFC 9113 §8.4: push is only accepted when this client enabled it.
    if (!enable_push_) {
        fail_connection(http2_error_code::protocol_error);
        return 0;
    }
    const auto associated_id = static_cast<std::uint32_t>(frame.hd.stream_id);
    const auto promised_id = static_cast<std::uint32_t>(frame.push_promise.promised_stream_id);
    last_peer_stream_id_ = promised_id;
    highest_peer_stream_id_ = (std::max)(highest_peer_stream_id_, promised_id);
    const auto* parent = streams_.find(associated_id);
    // The promised request decodes into scratch storage; the pushed stream's own
    // receive state awaits the response.
    push_scratch_.emplace(promised_id);
    push_associated_stream_id_ = associated_id;
    // PUSH_PROMISE carries a complete request without content.
    (void)push_scratch_->record_remote_head_end_stream();
    auto& block = header_block_.emplace(header_block_kind::push_promise, *push_scratch_);
    block.reject_connected_tunnel_ = parent != nullptr && parent->tunnel().open() != nullptr;
    return 0;
}

int http2_connection::on_header(const nghttp2_frame& frame, std::string_view name, std::string_view value) {
    if (!header_block_.has_value() || local_connection_state_.fatal_failure() != nullptr) {
        return 0;
    }
    observe_header(http2_header_observation_phase::field, name, value);
    if (accept_header_field(name, value)) {
        return 0;
    }
    // Malformed: reset now; nghttp2 decodes the rest of the block for HPACK
    // without further callbacks.
    const auto kind = header_block_->kind_;
    header_block_.reset();
    if (kind == header_block_kind::push_promise) {
        push_scratch_.reset();
        reset_stream_for_peer_error(
            static_cast<std::uint32_t>(frame.push_promise.promised_stream_id), http2_error_code::protocol_error);
    } else {
        reject_header_block(static_cast<std::uint32_t>(frame.hd.stream_id), kind == header_block_kind::request);
    }
    return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
}

bool http2_connection::accept_header_field(std::string_view name, std::string_view value) {
    auto& block = *header_block_;
    switch (block.kind_) {
        case header_block_kind::request:
        case header_block_kind::push_promise:
            return http2_on_decoded_initial_header(block.context_, name, value);
        case header_block_kind::request_trailers:
            return http2_on_decoded_request_trailer(block.context_, name, value);
        case header_block_kind::response:
            return accept_response_header(block.context_, block.informational_fields_, block.status_,
                block.content_semantics_, block.saw_regular_, name, value);
        case header_block_kind::response_trailers:
            return http2_on_decoded_response_trailer(block.context_, name, value);
    }
    return false;
}

void http2_connection::reject_header_block(std::uint32_t stream_id, bool malformed_request_head) {
    reset_stream_for_peer_error(stream_id, http2_error_code::protocol_error);
    if (!malformed_request_head) {
        return;
    }
    // A peer can otherwise keep one connection alive while forcing the server to
    // decode and reset malformed request heads across almost the whole stream-id
    // space. Valid exchanges do not refill this budget.
    if (++malformed_request_streams_ >= max_malformed_request_streams) {
        fail_connection(http2_error_code::enhance_your_calm);
    }
}

void http2_connection::finish_header_block(const nghttp2_frame& frame) {
    observe_header(http2_header_observation_phase::end);
    if (!header_block_.has_value()) {
        return;
    }
    auto block = std::move(*header_block_);
    header_block_.reset();
    auto& stream = block.context_.stream_;
    switch (block.kind_) {
        case header_block_kind::request:
            if (!finish_request_head(stream)) {
                reject_header_block(stream.id(), true);
                return;
            }
            emit_request_headers(stream);
            return;
        case header_block_kind::response:
            if (!finish_response_head(stream, block)) {
                reject_header_block(stream.id(), false);
            }
            return;
        case header_block_kind::request_trailers:
        case header_block_kind::response_trailers:
            if (!finish_trailers(stream)) {
                reject_header_block(stream.id(), false);
            }
            return;
        case header_block_kind::push_promise:
            finish_push_promise(frame, block);
            return;
    }
}

bool http2_connection::finish_request_head(http2_stream_state& stream) {
    if (!stream.has_method()) {
        return false;
    }
    if (stream.has_protocol()) {
        // RFC 8441 §4: Extended CONNECT requires our SETTINGS_ENABLE_CONNECT_PROTOCOL.
        if (!received_peer_settings_ ||
            stream.request_known_method() != http_known_method::connect || !stream.has_scheme() ||
            !stream.has_path() || !stream.has_authority() ||
            !is_valid_binary_request_authority(stream.request_scheme(), stream.request_authority()) ||
            !http_binary_request_path_matches_method_and_scheme(
                stream.request_known_method(), stream.request_scheme(), stream.request_path()) ||
            !http2_connect_content_length_acceptable(stream)) {
            return false;
        }
        // RFC 8441 defines websocket extended CONNECT only for HTTP(S) URI
        // schemes. Other extended protocols retain the full RFC 3986 space.
        if (!is_valid_extended_connect_scheme(stream.request_protocol(), stream.request_scheme())) {
            return false;
        }
        if (!stream.begin_extended_connect()) {
            return false;
        }
    } else if (stream.request_known_method() == http_known_method::connect) {
        request_target_view connect_target;
        if (!stream.has_authority() || stream.has_scheme() || stream.has_path() ||
            !http2_connect_content_length_acceptable(stream) ||
            !parse_request_target(
                http_known_method::connect, stream.request_authority(), connect_target)) {
            return false;
        }
        if (!stream.begin_standard_connect()) {
            return false;
        }
    } else if (!stream.has_scheme() || !stream.has_path() ||
               (stream.has_authority() && !is_valid_binary_request_authority(
                                              stream.request_scheme(), stream.request_authority())) ||
               !http_binary_request_path_matches_method_and_scheme(
                   stream.request_known_method(), stream.request_scheme(), stream.request_path()) ||
               !http2_has_required_request_authority(stream)) {
        return false;
    }
    const bool remote_head_finalized = stream.tunnel().pending() != nullptr
                                           ? stream.finalize_remote_connect_head()
                                           : stream.finalize_remote_content_head();
    if (!remote_head_finalized) {
        return false;
    }
    return !http2_remote_peer_half_closed(stream) || stream.remote_content().terminal_length_valid();
}

bool http2_connection::finish_response_head(http2_stream_state& stream, header_block_type& block) {
    if (!block.status_) {
        return false;
    }
    if (block.status_->status().is_informational()) {
        // A 1xx cannot carry END_STREAM. The remote state stays head-pending so
        // the next HEADERS decodes as another head.
        if (stream.remote_receive().head_end_stream_pending() != nullptr) {
            return false;
        }
        stream.mark_informational_response();
        stream.activate_push();
        auto head = http_client_response_head_access::make(*block.status_, http_protocol_version::http2);
        if (!block.informational_fields_.empty()) {
            http_client_response_head_access::headers(head) = std::move(block.informational_fields_);
        }
        std::optional<http_client_request_content_signal> signal;
        if (block.status_->status() == http_status::continue_value && stream.release_request_continue()) {
            signal = http_client_request_content_signal::continue_value;
        }
        publish_event(http2_event(http2_informational_head_event(stream.id(), std::move(head), signal)));
        return true;
    }
    (void)stream.cancel_pending_request_continue();
    if (!stream.set_response_status(*block.status_)) {
        return false;
    }
    // A 205 sender MUST NOT generate content (RFC 9110 §15.3.6), but DATA
    // framing stays unambiguous: Content-Length still binds the DATA sum and any
    // content is consumed and discarded by the DATA path.
    if (block.content_semantics_ == http_response_content_semantics::without_content &&
        !stream.select_remote_content_metadata_only()) {
        return false;
    }
    if (stream.tunnel().pending() != nullptr) {
        if (block.content_semantics_ == http_response_content_semantics::connect_tunnel) {
            if (!stream.accept_connect()) {
                return false;
            }
            stream.begin_local_content_unbounded();
            (void)stream.open_local_connect_tunnel();
        } else {
            if (!stream.reject_connect()) {
                return false;
            }
            if (http2_remote_peer_half_closed(stream) && !stream.remote_content().terminal_length_valid()) {
                return false;
            }
            // A rejected CONNECT ends the client's send half with an empty
            // DATA(END_STREAM); nghttp2 holds it until flow control admits it.
            nghttp2_data_provider2 provider{};
            provider.source.ptr = nullptr;
            provider.read_callback = &http2_connection::read_outbound_data;
            check_nghttp2(nghttp2_submit_data2(
                session_.get(), NGHTTP2_FLAG_END_STREAM, static_cast<std::int32_t>(stream.id()), &provider));
            if (can_send_empty_end_stream(stream.id())) {
                (void)stream.reject_local_connect();
            }
        }
    } else if (!stream.finalize_remote_content_head()) {
        return false;
    }
    if (http2_remote_peer_half_closed(stream) && !stream.remote_content().terminal_length_valid()) {
        return false;
    }
    stream.activate_push();
    emit_request_headers(stream);
    return true;
}

bool http2_connection::finish_trailers(http2_stream_state& stream) {
    if (!stream.remote_content().terminal_length_valid() || !stream.finish_remote_content()) {
        return false;
    }
    publish_event(http2_event(http2_message_end_record{stream.id()}));
    release_local_request_stream_if_closed(stream);
    return true;
}

void http2_connection::finish_push_promise(const nghttp2_frame& frame, header_block_type& block) {
    const auto promised_id = static_cast<std::uint32_t>(frame.push_promise.promised_stream_id);
    const auto associated_id = push_associated_stream_id_;
    struct scratch_guard final {
        std::optional<http2_stream_state>& scratch_;
        ~scratch_guard() {
            scratch_.reset();
        }
    } guard{push_scratch_};
    auto& scratch = *push_scratch_;
    std::optional<http2_error_code> refusal;
    if (block.reject_connected_tunnel_) {
        // RFC 9113 §8.5: on a connected stream only DATA, RST_STREAM,
        // WINDOW_UPDATE and PRIORITY are allowed. The promised stream is
        // reserved by the frame (§6.6), so it is cancelled explicitly.
        refusal = http2_error_code::cancel;
    } else if (!finish_request_head(scratch) ||
               (scratch.request_method() != "GET" && scratch.request_method() != "HEAD")) {
        refusal = http2_error_code::protocol_error;
    }
    auto* stream = refusal ? nullptr : create_stream(promised_id);
    if (stream == nullptr) {
        if (block.reject_connected_tunnel_) {
            reset_stream_for_peer_error(associated_id, http2_error_code::protocol_error);
        }
        check_nghttp2(nghttp2_submit_rst_stream(session_.get(), NGHTTP2_FLAG_NONE,
            static_cast<std::int32_t>(promised_id),
            static_cast<std::uint32_t>(refusal.value_or(http2_error_code::enhance_your_calm))));
        return;
    }
    http_push_request request;
    request.method_ = scratch.request_method();
    request.scheme_ = scratch.request_scheme();
    request.authority_ = scratch.request_authority();
    request.path_ = scratch.request_path();
    request.headers_.reserve(scratch.remote_header_count() + static_cast<std::size_t>(scratch.has_cookie()));
    for (std::size_t i = 0; i < scratch.remote_header_count(); ++i) {
        const auto field = scratch.remote_header_at(i);
        request.headers_.emplace_back(field.name_, field.value_);
    }
    if (scratch.has_cookie()) {
        request.headers_.emplace_back("cookie", scratch.request_cookie());
    }
    pin_stream(promised_id);
    stream->take_request_target_from(scratch);
    stream->begin_local_content_forbidden();
    (void)stream->commit_local_head_end_stream();
    stream->reserve_push(http2_push_reservation::remote);
    publish_event(http2_event(http2_push_promise_event{associated_id, promised_id, std::move(request)}));
}

void http2_connection::cancel_push_promise_on(std::uint32_t associated_stream_id) {
    if (!header_block_.has_value() || header_block_->kind_ != header_block_kind::push_promise ||
        push_associated_stream_id_ != associated_stream_id) {
        return;
    }
    const auto promised_id = push_scratch_->id();
    header_block_.reset();
    push_scratch_.reset();
    // RFC 9113 §8.4: a client may cancel a promise it will not use.
    submit_owner_reset(promised_id, http2_error_code::cancel);
}

void http2_connection::emit_request_headers(http2_stream_state& stream) {
    // A later frame in the same input batch can close this stream before the
    // public request event acquires its lease. Keep that event's storage alive.
    if (role_ == http2_role::server) {
        if (!priorities_.empty()) {
            const auto priority = std::ranges::find_if(priorities_, [&stream](const auto& entry_value) noexcept {
                return entry_value.stream_id_ == stream.id();
            });
            if (priority != priorities_.end()) {
                stream.set_priority_update(priority->fields_);
                priorities_.erase(priority);
            }
        }
        pin_stream(stream.id());
    }
    const auto request_content_signal = stream.request_content_canceled()
                                            ? std::optional<http_client_request_content_signal>(
                                                  http_client_request_content_signal::exchange_complete)
                                            : std::nullopt;
    const bool body_open = !http2_remote_peer_half_closed(stream) &&
                           (role_ == http2_role::client || stream.tunnel().pending() == nullptr);
    publish_event(http2_event(http2_message_head_record{stream.id(), request_content_signal, body_open, http2_remote_peer_half_closed(stream)}));
    if (role_ == http2_role::server && stream.tunnel().pending() != nullptr) {
        // CONNECT has no request content; the typed remote state owns any later
        // tunnel-end signal, so a generic message_end would be misleading.
        return;
    }
    if (role_ == http2_role::client && stream.tunnel().open() != nullptr) {
        if (http2_remote_peer_half_closed(stream)) {
            publish_event(http2_event(http2_tunnel_end_event(stream.id())));
        }
        release_local_request_stream_if_closed(stream);
        return;
    }
    if (http2_remote_peer_half_closed(stream)) {
        publish_event(http2_event(http2_message_end_record{stream.id()}));
    }
    release_local_request_stream_if_closed(stream);
}

// --- DATA ----------------------------------------------------------------------

int http2_connection::on_data_chunk_recv(std::uint8_t, std::uint32_t stream_id, std::string_view data) {
    auto* stream = streams_.find(stream_id);
    if (local_connection_state_.fatal_failure() != nullptr || stream == nullptr || stream->is_aborted()) {
        // DATA already in flight when this endpoint reset the stream still
        // consumed flow-control credit; return it at once.
        consume_received(stream_id, data.size());
        return 0;
    }
    const auto drop = [this, stream_id, &data](http2_error_code error) {
        reset_stream_for_peer_error(stream_id, error);
        consume_received(stream_id, data.size());
        return 0;
    };
    const auto& remote = stream->remote_receive();
    if (remote.head_pending() != nullptr || remote.head_end_stream_pending() != nullptr) {
        return drop(http2_error_code::protocol_error);
    }
    if (remote.connect_pending() != nullptr) {
        // RFC 9113 §8.5 makes every DATA after a CONNECT request tunnel data, and
        // RFC 9298 §5 (as updated by RFC 9931 §6.3) lets a client send it before
        // the 2xx. The owner holds the credit until it consumes the bytes after
        // accepting.
        (void)stream->mark_remote_connect_tunnel_data();
        stream->add_window_debt(static_cast<std::uint32_t>(data.size()));
        publish_event(http2_event(http2_tunnel_data_record{stream_id, data, static_cast<std::uint32_t>(data.size())}));
        return 0;
    }
    if (remote.connect_rejected_awaiting_end_stream() != nullptr) {
        // A rejected CONNECT's further DATA is discarded and its credit returned.
        consume_received(stream_id, data.size());
        return 0;
    }
    if (remote.tunnel_open() != nullptr) {
        stream->add_window_debt(static_cast<std::uint32_t>(data.size()));
        publish_event(http2_event(http2_tunnel_data_record{stream_id, data, static_cast<std::uint32_t>(data.size())}));
        return 0;
    }
    if (remote.content_open() == nullptr) {
        return drop(http2_error_code::stream_closed);
    }
    switch (stream->account_remote_content(data.size())) {
        case http2_remote_content_accounting_result::accepted:
            break;
        case http2_remote_content_accounting_result::counter_overflow:
            return drop(http2_error_code::cancel);
        default:
            return drop(http2_error_code::protocol_error);
    }
    // HEAD-like responses carry metadata only; a 205 response's content is
    // consumed and discarded because its sender MUST NOT generate any (RFC 9110
    // §15.3.6). Both keep flow control and Content-Length accounting intact.
    const auto* response_status = stream->response_status();
    const bool discarded = stream->remote_content().metadata_only_without_length() != nullptr ||
                           stream->remote_content().metadata_only_known_length() != nullptr ||
                           (response_status != nullptr && response_status->status() == http_status::reset_content);
    if (discarded) {
        consume_received(stream_id, data.size());
        return 0;
    }
    // New capacity is advertised only after the owner consumed or copied the
    // borrowed bytes, preserving backpressure.
    stream->add_window_debt(static_cast<std::uint32_t>(data.size()));
    publish_event(http2_event(http2_message_data_record{stream_id, data, static_cast<std::uint32_t>(data.size())}));
    return 0;
}

void http2_connection::finish_data_frame(http2_stream_state& stream) {
    const auto stream_id = stream.id();
    const auto& remote = stream.remote_receive();
    if (remote.connect_pending() != nullptr) {
        (void)stream.finish_remote_pending_connect();
    } else if (remote.connect_rejected_awaiting_end_stream() != nullptr) {
        (void)stream.finish_remote_rejected_connect();
    } else if (remote.tunnel_open() != nullptr) {
        (void)stream.finish_remote_tunnel();
        publish_event(http2_event(http2_tunnel_end_event(stream_id)));
    } else if (remote.content_open() != nullptr) {
        if (!stream.remote_content().terminal_length_valid()) {
            // Content-Length mismatch (RFC 9113 §8.1.1): the delivered chunks
            // stand, the stream is malformed.
            reset_stream_for_peer_error(stream_id, http2_error_code::protocol_error);
            return;
        }
        (void)stream.finish_remote_content();
        publish_event(http2_event(http2_message_end_record{stream_id}));
    } else {
        reset_stream_for_peer_error(stream_id, http2_error_code::protocol_error);
        return;
    }
    release_local_request_stream_if_closed(stream);
}

}  // namespace ruvia::detail

namespace ruvia {

http2_setting_entry http2_received_settings::at(std::size_t index) const {
    if (index >= size_) {
        throw std::out_of_range("HTTP/2 received SETTINGS index out of range");
    }
    return entries_[index];
}

namespace {

[[nodiscard]] http_client_response_head response_head_from_stream(
    const detail::http2_stream_state& stream) {
    const auto* status = stream.response_status();
    if (status == nullptr) {
        throw std::logic_error("HTTP/2 final response event has no status");
    }
    auto result_value =
        detail::http_client_response_head_access::make(*status, http_protocol_version::http2);
    if (const auto* known = stream.remote_content().allowed_known_length()) {
        detail::http_client_response_head_access::content_length(result_value, known->declared_length());
    } else if (const auto* metadata = stream.remote_content().metadata_only_known_length()) {
        detail::http_client_response_head_access::content_length(result_value, metadata->declared_length());
    }
    const auto header_count = stream.remote_header_count();
    if (header_count != 0) {
        auto& headers = detail::http_client_response_head_access::headers(result_value);
        headers.reserve(header_count);
        for (std::size_t index = 0; index < header_count; ++index) {
            const auto field = stream.remote_header_at(index);
            headers.emplace_back(field.name_, field.value_);
        }
    }
    return result_value;
}

[[nodiscard]] http2_message_content_semantics message_content_semantics(
    const detail::http2_stream_state& stream) noexcept {
    const auto& content = stream.remote_content();
    return content.metadata_only_without_length() != nullptr ||
                   content.metadata_only_known_length() != nullptr
               ? http2_message_content_semantics::metadata_only
               : http2_message_content_semantics::content;
}

}  // namespace

// Intrusively reference-counted so escaped events or credits can still inspect
// or abandon stream state after the public http2_connection facade is destroyed.
namespace detail {

class http2_connection_owner_endpoint final {
public:
    http2_connection_owner_endpoint(
        http2_role role, const http2_connection_options& options)
        : connection_(role, options.enable_push_, options.receive_origin_advertisements_,
              options.receive_frame_metadata_, options.frame_observer_, options.header_observer_,
              options.observer_context_) {}
    http2_connection_owner_endpoint(const http2_connection_owner_endpoint&) = delete;
    http2_connection_owner_endpoint& operator=(const http2_connection_owner_endpoint&) = delete;

    void retain() noexcept {
        ++references_;
    }

    void release() noexcept {
        if (references_ == 0) {
            std::terminate();
        }
        if (--references_ == 0) {
            delete this;
        }
    }

    void abandon_request(std::uint32_t stream_id) noexcept {
        connection_.abandon_stream_lease(stream_id);
    }

    void return_credit(std::uint32_t stream_id, std::uint32_t bytes_value) noexcept {
        connection_.return_received_data(stream_id, bytes_value);
    }
    [[nodiscard]] ::ruvia::http2_event materialize_message_head_event(
        const detail::http2_message_head_record& value) {
        auto* stream = connection_.stream(value.stream_id_);
        if (connection_.role() == http2_role::client) {
            if (stream == nullptr) {
                throw std::logic_error("HTTP/2 response event has no stream");
            }
            return ::ruvia::http2_event::response_head(value.stream_id_,
                response_head_from_stream(*stream), value.request_content_signal_, !value.body_open_);
        }
        if (stream == nullptr) {
            throw std::logic_error("HTTP/2 request event has no stream");
        }
        connection_.pin_stream(value.stream_id_);
        const auto content = stream->request_content_indication();
        const auto& remote = stream->remote_receive();
        const bool connect_pending =
            remote.connect_pending() != nullptr || remote.connect_pending_end_stream() != nullptr;
        std::optional<std::size_t> declared_content_length;
        if (const auto* known = stream->remote_content().allowed_known_length(); known != nullptr) {
            declared_content_length = known->declared_length();
        }
        const http2_server_request_snapshot snapshot{
            .content_ = content,
            .declared_content_length_ = declared_content_length,
            .priority_update_ = stream->priority_update(),
            .body_open_ = value.body_open_,
            .connect_pending_ = connect_pending,
            .initial_end_stream_ = value.initial_end_stream_,
        };
        return ::ruvia::http2_event::request_head(
            this, value.stream_id_, stream->request_expectations(), content, snapshot);
    }

    template <typename event_record_type>
    [[nodiscard]] ::ruvia::http2_event materialize_public_http2_event(event_record_type& value) {
        using value_type = std::decay_t<event_record_type>;
        const bool client = connection_.role() == http2_role::client;
        if constexpr (std::is_same_v<value_type, detail::http2_message_head_record>) {
            return materialize_message_head_event(value);
        } else if constexpr (std::is_same_v<value_type, detail::http2_message_data_record>) {
            return ::ruvia::http2_event::message_body_chunk(
                this, value.stream_id_, value.bytes_, value.flow_control_bytes_);
        } else if constexpr (std::is_same_v<value_type, detail::http2_tunnel_data_record>) {
            return ::ruvia::http2_event::tunnel_data(
                this, value.stream_id_, value.bytes_, value.flow_control_bytes_);
        } else if constexpr (std::is_same_v<value_type, detail::http2_message_end_record>) {
            auto* stream = connection_.stream(value.stream_id_);
            if (stream == nullptr) {
                throw std::logic_error("HTTP/2 message end event has no stream");
            }
            auto result = ::ruvia::http2_event::message_end(
                value.stream_id_, stream->take_remote_trailers(), message_content_semantics(*stream));
            if (client) {
                connection_.release_stream_lease_after_remote_end(value.stream_id_);
            }
            return result;
        } else {
            if constexpr (std::is_same_v<value_type, http2_tunnel_end_event>) {
                if (client) {
                    connection_.release_stream_lease_after_remote_end(value.stream_id());
                }
            } else if constexpr (std::is_same_v<value_type, http2_stream_closed_event> ||
                                 std::is_same_v<value_type, http2_request_unprocessed_event>) {
                if (client) {
                    connection_.release_stream_lease(value.stream_id());
                }
            }
            return ::ruvia::http2_event(std::move(value));
        }
    }
    std::size_t references_{1};
    detail::http2_connection connection_;
};

}  // namespace detail

http2_received_data_credit::http2_received_data_credit(detail::http2_connection_owner_endpoint* endpoint,
    std::uint32_t stream_id, std::uint32_t bytes_value) noexcept
    : endpoint_(endpoint),
      stream_id_(stream_id),
      bytes_(bytes_value) {
    endpoint_->retain();
}

http2_received_data_credit::~http2_received_data_credit() {
    if (endpoint_ == nullptr) {
        return;
    }
    endpoint_->return_credit(stream_id_, bytes_);
    endpoint_->release();
}

http2_received_data_credit::http2_received_data_credit(http2_received_data_credit&& other) noexcept
    : endpoint_(std::exchange(other.endpoint_, nullptr)),
      stream_id_(std::exchange(other.stream_id_, 0)),
      bytes_(std::exchange(other.bytes_, 0)) {}

void http2_received_data_credit::merge(http2_received_data_credit&& other) {
    if (this == &other || !valid() || !other.valid()) {
        throw std::logic_error("invalid HTTP/2 received data credit");
    }
    if (endpoint_ != other.endpoint_ || stream_id_ != other.stream_id_) {
        throw std::logic_error("HTTP/2 received data credits belong to different streams");
    }
    if (bytes_ > (std::numeric_limits<std::uint32_t>::max)() - other.bytes_) {
        throw std::overflow_error("HTTP/2 received data credit overflow");
    }
    bytes_ += other.bytes_;
    auto* endpoint = std::exchange(other.endpoint_, nullptr);
    other.stream_id_ = 0;
    other.bytes_ = 0;
    endpoint->release();
}

http2_request_head_event::http2_request_head_event(detail::http2_connection_owner_endpoint* endpoint,
    std::uint32_t stream_id, http_request_expectations expectations,
    http_request_content_indication content, http2_server_request_snapshot snapshot) noexcept
    : stream_id_(stream_id),
      snapshot_(snapshot),
      expectations_(expectations),
      content_(content),
      endpoint_(endpoint) {
    endpoint_->retain();
}

http2_request_head_event::~http2_request_head_event() {
    if (endpoint_ == nullptr) {
        return;
    }
    endpoint_->abandon_request(stream_id_);
    endpoint_->release();
}

http2_request_head_event::http2_request_head_event(http2_request_head_event&& other) noexcept
    : stream_id_(std::exchange(other.stream_id_, 0)),
      snapshot_(other.snapshot_),
      expectations_(other.expectations_),
      content_(other.content_),
      endpoint_(std::exchange(other.endpoint_, nullptr)) {}

http2_server_request_view http2_request_head_event::request_view() const& noexcept {
    if (endpoint_ == nullptr || stream_id_ == 0) {
        return {};
    }
    return endpoint_->connection_.server_request_view(stream_id_).value_or(http2_server_request_view{});
}

http_request http2_request_head_event::build_request(borrowed_text body) const& {
    if (endpoint_ == nullptr || stream_id_ == 0) {
        throw std::logic_error("HTTP/2 request head event is not active");
    }
    auto* stream = endpoint_->connection_.stream(stream_id_);
    if (stream == nullptr) {
        throw std::logic_error("missing HTTP/2 request stream");
    }
    const auto* pending = stream->tunnel().pending();
    const detail::binary_request_materialization_spec spec{
        .version_ = http_protocol_version::http2,
        .target_form_ = http_request_target_form::http2,
        .method_ = stream->request_method(),
        .scheme_ = stream->request_scheme(),
        .authority_ = stream->request_authority(),
        .host_ = detail::http2_request_host_field(*stream),
        .path_ = stream->request_path(),
        .protocol_ = stream->request_protocol(),
        .cookie_ = stream->request_cookie(),
        .body_ = body.view(),
        .regular_header_count_ = stream->remote_header_count(),
        .has_authority_ = stream->has_authority(),
        .has_host_ = stream->has_host(),
        .has_path_ = stream->has_path(),
        .has_cookie_ = stream->has_cookie(),
        .standard_connect_ = pending != nullptr && pending->form() == detail::http2_connect_form::standard,
        .failure_message_ = "validated HTTP/2 request cannot be materialized",
    };
    return detail::materialize_binary_http_request(spec, [stream](std::size_t index) noexcept {
        const auto header_value = stream->remote_header_at(index);
        return std::pair{http_header_view{header_value.name_, header_value.value_}, header_value.kind_};
    });
}
void http2_connection::impl_deleter_type::operator()(detail::http2_connection_owner_endpoint* value) const noexcept {
    if (value != nullptr) {
        value->release();
    }
}

http2_connection::http2_connection(http2_role role, const http2_connection_options& options) {
    impl_.reset(new detail::http2_connection_owner_endpoint(role, options));
    impl_->connection_.begin_connection();
}
http2_connection http2_connection::server(http2_connection_options options) {
    // Server push (RFC 9113 section 8.4) and ORIGIN receipt (RFC 8336 section 2)
    // are client roles; a server configured with them would silently ignore them.
    if (options.enable_push_ || options.receive_origin_advertisements_) {
        throw std::invalid_argument("HTTP/2 server connection does not accept client-only options");
    }
    return http2_connection(http2_role::server, options);
}
http2_connection http2_connection::client(http2_connection_options options) {
    return http2_connection(http2_role::client, options);
}
http2_connection::~http2_connection() = default;
http2_connection::http2_connection(http2_connection&&) noexcept = default;
http2_connection& http2_connection::operator=(http2_connection&&) noexcept = default;

websocket_handshake_validation_result validate_http2_websocket_handshake(
    http2_connection& connection, std::uint32_t stream_id,
    const http_request& request,
    websocket_handshake_validation_options options) noexcept {
    const auto* stream = connection.impl_->connection_.stream(stream_id);
    if (stream == nullptr) {
        return websocket_handshake_validation_result::make_invalid_request();
    }
    return detail::validate_http2_websocket_handshake(*stream, request, options);
}

std::variant<websocket_extended_connect_negotiation, http2_websocket_handshake_submit_error>
http2_connection::submit_websocket_handshake(
    std::uint32_t stream_id, const http_request& request,
    const websocket_handshake_validation_result& validation,
    websocket_server_handshake_options options) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.submit_websocket_handshake(
        stream_id, request, validation, options);
}

http2_role http2_connection::role() const noexcept {
    return impl_->connection_.role();
}
bool http2_connection::header_block_in_progress() const noexcept {
    return impl_->connection_.header_block_in_progress();
}
bool http2_connection::received_peer_settings() const noexcept {
    return impl_->connection_.received_peer_settings();
}
http2_feed_result http2_connection::feed(std::string_view input) {
    auto& connection = impl_->connection_;
    connection.retry_deferred_stream_leases();
    const auto result_value = connection.feed(input);
    connection.retry_deferred_stream_leases();
    return result_value;
}

std::optional<http2_event> http2_connection::next_event() {
    auto& connection = impl_->connection_;
    connection.retry_deferred_stream_leases();
    auto* event = connection.peek_event();
    if (event == nullptr) {
        return std::nullopt;
    }
    auto result_value = std::visit(
        [this](auto& value) {
            return impl_->materialize_public_http2_event(value);
        },
        event->value());
    connection.consume_event();
    return std::optional<http2_event>(std::move(result_value));
}

http2_output_batch_result http2_connection::take_output_batch(std::size_t max_bytes,
    std::string& output, http2_data_output_observer_type observer, void* observer_context) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.take_output_batch(max_bytes, output, observer, observer_context);
}
bool http2_connection::wants_write() const noexcept {
    return impl_->connection_.wants_write();
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_request_head(
    const http2_regular_request_head_view& request) {
    impl_->connection_.retry_deferred_stream_leases();
    std::optional<std::string_view> authority;
    if (request.authority_) {
        authority = request.authority_->view();
    }
    auto result_value = impl_->connection_.submit_regular_request_head(request.method_.view(),
        request.scheme_.view(), authority, request.target_.view(),
        static_cast<std::span<const http_header_view>>(request.headers_), request.content_,
        request.expectation_, request.field_source_);
    impl_->connection_.lease_submitted_request(result_value);
    return result_value;
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_request_head(
    const http2_connect_request_head_view& request) {
    impl_->connection_.retry_deferred_stream_leases();
    auto result_value = impl_->connection_.submit_connect_request_head(
        request.authority_.view(), static_cast<std::span<const http_header_view>>(request.headers_),
        request.field_source_);
    impl_->connection_.lease_submitted_request(result_value);
    return result_value;
}

std::variant<std::uint32_t, http2_request_head_submit_error> http2_connection::submit_request_head(
    const http2_extended_connect_request_head_view& request) {
    impl_->connection_.retry_deferred_stream_leases();
    auto result_value = impl_->connection_.submit_extended_connect_request_head(request.protocol_.view(),
        request.scheme_.view(), request.authority_.view(), request.target_.view(),
        static_cast<std::span<const http_header_view>>(request.headers_), request.field_source_);
    impl_->connection_.lease_submitted_request(result_value);
    return result_value;
}

http2_data_submit_result http2_connection::submit_data(std::uint32_t stream_id,
    std::string_view bytes_value, http2_end_stream end_stream, std::size_t max_payload_bytes) {
    auto& connection = impl_->connection_;
    const auto result_value = connection.submit_data(stream_id, bytes_value, end_stream, max_payload_bytes);
    connection.retry_deferred_stream_lease(stream_id);
    connection.retry_deferred_stream_leases();
    return result_value;
}
http2_request_content_release_status http2_connection::release_request_content(
    std::uint32_t stream_id) noexcept {
    return impl_->connection_.release_request_content(stream_id);
}
http2_submit_status http2_connection::submit_connect_response_head(std::uint32_t stream_id, const http_response& response) {
    return impl_->connection_.submit_connect_response_head(stream_id, response);
}

http2_submit_status http2_connection::submit_interim_response_head(
    std::uint32_t stream_id, const http_interim_response_head& response) {
    return impl_->connection_.submit_interim_response_head(stream_id, response);
}

std::variant<http_buffered_response_write_plan, http2_response_head_submit_error>
http2_connection::submit_response_head(
    std::uint32_t stream_id, const http_response& response,
    http_buffered_response_write_plan write_plan) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.submit_response_head(stream_id, response, std::move(write_plan));
}

std::variant<http_response_stream_commit_plan, http2_response_head_submit_error>
http2_connection::submit_streaming_response_head(
    std::uint32_t stream_id, http_response response,
    http_response_trailer_intent trailer_intent) {
    impl_->connection_.retry_deferred_stream_leases();
    return impl_->connection_.submit_streaming_response_head(
        stream_id, std::move(response), trailer_intent);
}

http2_finish_status http2_connection::finish_request(std::uint32_t stream_id,
    std::span<const http_header_view> trailers) {
    auto& connection = impl_->connection_;
    const auto status = connection.finish_request(stream_id, trailers);
    connection.retry_deferred_stream_lease(stream_id);
    connection.retry_deferred_stream_leases();
    return status;
}

http2_finish_status http2_connection::finish_response(
    std::uint32_t stream_id, const http_response_trailer_section& trailers) {
    auto& connection = impl_->connection_;
    const auto status = connection.finish_response(stream_id, trailers);
    connection.retry_deferred_stream_lease(stream_id);
    connection.retry_deferred_stream_leases();
    return status;
}

std::variant<http2_request_head_event, http2_push_submit_error> http2_connection::submit_push_request(
    std::uint32_t associated_stream_id, http_push_request_view request) {
    impl_->connection_.retry_deferred_stream_leases();
    const auto id = impl_->connection_.submit_push_promise(associated_stream_id, request);
    if ((id.index() != 0)) {
        return std::get<1>(id);
    }
    impl_->connection_.pin_stream(std::get<0>(id));
    return http2_request_head_event(impl_.get(), std::get<0>(id), {},
        http_request_content_indication::no_content, {});
}

http2_submit_status http2_connection::submit_origin_advertisement(std::span<const std::string_view> origins) {
    return impl_->connection_.submit_origin_advertisement(origins);
}
http2_submit_status http2_connection::submit_alternative_service_advertisement(std::uint32_t stream_id,
    std::string_view origin, std::string_view field_value) {
    return impl_->connection_.submit_alternative_service_advertisement(stream_id, origin, field_value);
}
http2_submit_status http2_connection::submit_priority_update(std::uint32_t stream_id, http_priority_fields fields_value) {
    return impl_->connection_.submit_priority_update(stream_id, fields_value);
}

http2_submit_status http2_connection::submit_reset(std::uint32_t stream_id, http2_error_code error) {
    auto& connection = impl_->connection_;
    const auto status = connection.submit_reset(stream_id, error);
    if (status == http2_submit_status::accepted && connection.role() == http2_role::client) {
        // Owner-originated resets deliberately produce no terminal event. The
        // public client facade must therefore release the pin established by
        // submit_request_head() here rather than waiting for next_event().
        connection.release_stream_lease(stream_id);
    }
    connection.retry_deferred_stream_lease(stream_id);
    connection.retry_deferred_stream_leases();
    return status;
}
http2_submit_status http2_connection::submit_ping() {
    return impl_->connection_.submit_ping();
}
http2_submit_status http2_connection::submit_max_concurrent_streams(std::uint32_t limit) {
    return impl_->connection_.submit_max_concurrent_streams(limit);
}
std::size_t http2_connection::available_request_streams(std::size_t local_limit) const noexcept {
    return impl_->connection_.available_local_request_streams(local_limit);
}

std::size_t http2_connection::pending_data_output_bytes(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.pending_data_output_bytes(stream_id);
}
std::optional<http2_send_window_state> http2_connection::send_window_state(
    std::uint32_t stream_id) const noexcept {
    return impl_->connection_.send_window_state(stream_id);
}
bool http2_connection::stream_aborted(std::uint32_t stream_id) const noexcept {
    return impl_->connection_.stream_aborted(stream_id);
}
http2_stream_receive_status http2_connection::stream_receive_status(
    std::uint32_t stream_id) const noexcept {
    return impl_->connection_.stream_receive_status(stream_id);
}
http2_server_request_release_status http2_connection::release(http2_request_head_event&& request) {
    if (request.endpoint_ == nullptr || request.stream_id_ == 0 ||
        request.endpoint_ != impl_.get()) {
        throw std::logic_error("invalid HTTP/2 server request lease");
    }
    impl_->connection_.retry_deferred_stream_leases();
    const bool live = impl_->connection_.stream(request.stream_id_) != nullptr;
    if (live) {
        impl_->connection_.release_stream_lease(request.stream_id_);
    }
    auto* endpoint = std::exchange(request.endpoint_, nullptr);
    request.stream_id_ = 0;
    endpoint->release();
    return live ? http2_server_request_release_status::released : http2_server_request_release_status::closed;
}
void http2_connection::begin_drain() {
    impl_->connection_.begin_drain();
}
void http2_connection::finish_drain() {
    impl_->connection_.finish_drain();
}

void http2_connection::terminate(http2_error_code error) {
    impl_->connection_.terminate(error);
}
bool http2_connection::draining() const noexcept {
    return impl_->connection_.draining();
}
bool http2_connection::drain_final() const noexcept {
    return impl_->connection_.drain_final();
}
std::optional<http2_error_code> http2_connection::connection_error() const noexcept {
    return impl_->connection_.connection_error();
}

std::variant<std::uint32_t, http2_request_head_submit_error> submit_http2_websocket_client_request(
    http2_connection& connection, const websocket_client_negotiation& negotiation, std::string_view scheme,
    std::string_view authority, std::string_view target, borrowed_http_headers headers, http_request_field_source source) {
    const auto staged_headers = detail::stage_websocket_extended_connect_fields(negotiation, headers.view());
    return connection.submit_request_head(http2_extended_connect_request_head_view{
        .protocol_ = "websocket",
        .scheme_ = scheme,
        .authority_ = authority,
        .target_ = target,
        .headers_ = staged_headers.view(),
        .field_source_ = source,
    });
}

}  // namespace ruvia

namespace ruvia {
namespace detail {

class hpack_decoder final {
public:
    hpack_decoder()
        : memory_table_(make_c_library_mem_table<nghttp2_mem>(memory_)) {
        const int result = nghttp2_hd_inflate_new2(&inflater_, &memory_table_);
        if (result == NGHTTP2_ERR_NOMEM) {
            memory_.rethrow_allocation_failure();
        }
        if (result != 0) {
            throw std::runtime_error("nghttp2 failed to create an HPACK inflater");
        }
    }
    ~hpack_decoder() {
        nghttp2_hd_inflate_del(inflater_);
    }
    hpack_decoder(const hpack_decoder&) = delete;
    hpack_decoder& operator=(const hpack_decoder&) = delete;

    void set_max_dynamic_table_size(std::uint32_t bytes) {
        if (decoding_) {
            throw std::logic_error("HPACK table size cannot change during decoding");
        }
        const int result = nghttp2_hd_inflate_change_table_size(inflater_, bytes);
        if (result == NGHTTP2_ERR_NOMEM) {
            memory_.rethrow_allocation_failure();
        }
        if (result != 0) {
            throw std::invalid_argument("invalid HPACK dynamic table size");
        }
    }

    [[nodiscard]] std::optional<hpack_decode_error> decode(
        std::string_view block, void* target, bool (*callback)(void*, std::string_view, std::string_view)) {
        if (decoding_) {
            throw std::logic_error("nested HPACK decode on the same decoder");
        }
        if (poisoned_) {
            return hpack_decode_error::compression_error;
        }
        struct decoding_guard final {
            bool& decoding_;
            explicit decoding_guard(bool& decoding) noexcept
                : decoding_(decoding) {
                decoding_ = true;
            }
            ~decoding_guard() {
                decoding_ = false;
            }
            decoding_guard(const decoding_guard&) = delete;
            decoding_guard& operator=(const decoding_guard&) = delete;
        } guard(decoding_);

        const auto* cursor = reinterpret_cast<const std::uint8_t*>(block.data());
        auto remaining = block.size();
        bool rejected = false;
        for (;;) {
            nghttp2_nv field{};
            int flags = 0;
            const auto consumed = nghttp2_hd_inflate_hd3(inflater_, &field, &flags, cursor, remaining, 1);
            if (consumed < 0) {
                // The table may already hold part of this block; it can no
                // longer be trusted to mirror the peer's encoder.
                poisoned_ = true;
                if (consumed == NGHTTP2_ERR_NOMEM) {
                    memory_.rethrow_allocation_failure();
                }
                return hpack_decode_error::compression_error;
            }
            cursor += consumed;
            remaining -= static_cast<std::size_t>(consumed);
            if ((flags & NGHTTP2_HD_INFLATE_EMIT) != 0 && !rejected &&
                !callback(target, std::string_view(reinterpret_cast<const char*>(field.name), field.namelen),
                    std::string_view(reinterpret_cast<const char*>(field.value), field.valuelen))) {
                rejected = true;
            }
            if ((flags & NGHTTP2_HD_INFLATE_FINAL) != 0) {
                nghttp2_hd_inflate_end_headers(inflater_);
                break;
            }
        }
        return rejected ? std::optional(hpack_decode_error::callback_rejected) : std::nullopt;
    }

private:
    http_c_library_memory memory_;
    nghttp2_mem memory_table_;
    nghttp2_hd_inflater* inflater_{nullptr};
    bool decoding_{false};
    bool poisoned_{false};
};

namespace {

// Uses a per-call deflater so encoding maintains no shared state.
class hpack_field_deflater final {
public:
    hpack_field_deflater()
        : memory_table_(make_c_library_mem_table<nghttp2_mem>(memory_)) {
        const int result = nghttp2_hd_deflate_new2(&deflater_, 0, &memory_table_);
        if (result == NGHTTP2_ERR_NOMEM) {
            memory_.rethrow_allocation_failure();
        }
        if (result != 0) {
            throw std::runtime_error("nghttp2 failed to create an HPACK deflater");
        }
        // A deflater whose table is smaller than RFC 7541's initial 4096 bytes
        // announces the reduction in its first block. Consume that update
        // here so every encoded field is self-contained.
        std::array<std::uint8_t, 8> announcement{};
        if (nghttp2_hd_deflate_hd2(deflater_, announcement.data(), announcement.size(), nullptr, 0) < 0) {
            nghttp2_hd_deflate_del(deflater_);
            throw std::runtime_error("nghttp2 failed to prime the HPACK deflater");
        }
    }
    ~hpack_field_deflater() {
        nghttp2_hd_deflate_del(deflater_);
    }
    hpack_field_deflater(const hpack_field_deflater&) = delete;
    hpack_field_deflater& operator=(const hpack_field_deflater&) = delete;

    void encode(std::string& output, std::string_view name, std::string_view value, bool sensitive) {
        nghttp2_nv field{
            .name = reinterpret_cast<std::uint8_t*>(const_cast<char*>(name.data())),
            .value = reinterpret_cast<std::uint8_t*>(const_cast<char*>(value.data())),
            .namelen = name.size(),
            .valuelen = value.size(),
            .flags = static_cast<std::uint8_t>(sensitive ? NGHTTP2_NV_FLAG_NO_INDEX : NGHTTP2_NV_FLAG_NONE),
        };
        const auto bound = nghttp2_hd_deflate_bound(deflater_, &field, 1);
        if (bound > output.max_size() - output.size()) {
            throw std::length_error("HPACK field exceeds the output size limit");
        }
        const auto previous_size = output.size();
        output.resize(previous_size + bound);
        const auto encoded = nghttp2_hd_deflate_hd2(deflater_,
            reinterpret_cast<std::uint8_t*>(output.data() + previous_size), bound, &field, 1);
        if (encoded < 0) {
            output.resize(previous_size);
            if (encoded == NGHTTP2_ERR_NOMEM) {
                memory_.rethrow_allocation_failure();
            }
            throw std::length_error("nghttp2 rejected the HPACK field");
        }
        output.resize(previous_size + static_cast<std::size_t>(encoded));
    }

private:
    http_c_library_memory memory_;
    nghttp2_mem memory_table_;
    nghttp2_hd_deflater* deflater_{nullptr};
};
}  // namespace
}  // namespace detail

hpack_decoder::hpack_decoder()
    : impl_(std::make_unique<detail::hpack_decoder>()) {}
hpack_decoder::~hpack_decoder() = default;
hpack_decoder::hpack_decoder(hpack_decoder&&) noexcept = default;
hpack_decoder& hpack_decoder::operator=(hpack_decoder&&) noexcept = default;

void hpack_decoder::set_max_dynamic_table_size(hpack_table_size bytes_value) {
    impl_->set_max_dynamic_table_size(bytes_value.value());
}

std::optional<hpack_decode_error> hpack_decoder::decode_with_callback(
    std::string_view block, void* target, header_callback_type callback_value) {
    return impl_->decode(block, target, callback_value);
}

void hpack_encoder::encode_header(std::string& output, std::string_view name, std::string_view value) {
    // Resizing output may invalidate borrowed views; own them first.
    std::string owned_name;
    std::string owned_value;
    if (detail::http_text_aliases_string(name, output)) {
        owned_name.assign(name);
        name = owned_name;
    }
    if (detail::http_text_aliases_string(value, output)) {
        owned_value.assign(value);
        value = owned_value;
    }
    detail::hpack_field_deflater deflater;
    deflater.encode(output, name, value, detail::http_field_never_indexed(name));
}

void hpack_encoder::encode_status(std::string& output, http_status_code status) {
    const auto token = detail::http_status_code_token(status);
    encode_header(output, ":status", detail::http_status_code_token_view(token));
}

}  // namespace ruvia
