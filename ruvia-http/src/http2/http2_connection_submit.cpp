#include <algorithm>
#include <array>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include "ruvia/http/http2_framing.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_syntax.h"
#include "ruvia/http/http_header.h"
#include "ruvia/http/http_priority.h"
#include "ruvia/http/http_request_content_semantics.h"
#include "ruvia/http/http_request_target.h"
#include "ruvia/http/http_response_server.h"
#include "ruvia/http/url_encoding.h"

#include "http2/http2_connection.h"
#include "http_connection_fields.h"
#include "http_websocket_permessage_deflate.h"
#include "http_wire_sink.h"
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
