#include <cstdint>
#include <memory_resource>
#include <optional>
#include <string_view>
#include <utility>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_status.h"

#include "client/http_client_access.h"
#include "coding/http_content_length.h"
#include "http2/http2_connection.h"
#include "http2/http2_header_rules.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_request_headers.h"
#include "http2/http2_response_headers.h"

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

// ':status' is first and unique; interim metadata is retained separately
// without applying sender prohibitions.
[[nodiscard]] bool accept_response_header(http2_header_decode_context& context,
    std::pmr::vector<http_header>& informational_fields, std::optional<http_received_status>& status_value,
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
        informational_fields.emplace_back(name, value, informational_fields.get_allocator().resource());
        return true;
    }
    const auto kind = classify_request_header(name);
    // RFC 9110 §9.3.6: successful CONNECT ignores Content-Length semantics,
    // including invalid numerals; the field is retained for extension validators.
    if (kind == request_header_kind::content_length &&
        content_semantics != http_response_content_semantics::connect_tunnel) {
        http_content_length_state content_length;
        if (content_length.parse_field(value) != http_content_length_parse_status::ok ||
            !stream.declare_remote_content_length(*content_length.value())) {
            return false;
        }
    }
    return stream.append_remote_header(name, value, kind);
}

}  // namespace

bool http2_on_decoded_response_trailer(void* target, std::string_view name, std::string_view value) {
    auto& context_value = *static_cast<http2_header_decode_context*>(target);
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

// --- HEADERS / PUSH_PROMISE ---------------------------------------------------

int http2_connection::on_begin_headers(const nghttp2_frame& frame) {
    header_block_.reset();
    if (local_connection_state_.fatal_failure() != nullptr) {
        return 0;
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
        if (end_stream) {
            (void)stream->record_remote_head_end_stream();
        }
        header_block_.emplace(header_block_kind::request, *stream, resource_);
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
    header_block_.emplace(header_block_kind::request_trailers, *stream, resource_);
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
        header_block_.emplace(header_block_kind::response, *stream, resource_);
        return 0;
    }
    if (stream->tunnel().open() != nullptr || stream->remote_receive().content_open() == nullptr || !end_stream) {
        reset_stream_for_peer_error(stream_id, http2_error_code::protocol_error);
        return NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE;
    }
    header_block_.emplace(header_block_kind::response_trailers, *stream, resource_);
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
    const auto* parent = streams_.find(associated_id);
    // The promised request decodes into scratch storage; the pushed stream's own
    // receive state awaits the response.
    push_scratch_.emplace(promised_id, resource_);
    push_associated_stream_id_ = associated_id;
    // PUSH_PROMISE carries a complete request without content.
    (void)push_scratch_->record_remote_head_end_stream();
    auto& block = header_block_.emplace(header_block_kind::push_promise, *push_scratch_, resource_);
    block.reject_connected_tunnel_ = parent != nullptr && parent->tunnel().open() != nullptr;
    return 0;
}

int http2_connection::on_header(const nghttp2_frame& frame, std::string_view name, std::string_view value) {
    if (!header_block_.has_value() || local_connection_state_.fatal_failure() != nullptr) {
        return 0;
    }
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
            return http2_on_decoded_response_trailer(&block.context_, name, value);
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
            !http2_is_valid_request_authority(stream.request_scheme(), stream.request_authority()) ||
            !http2_request_path_matches_method_and_scheme(
                stream.request_known_method(), stream.request_scheme(), stream.request_path()) ||
            !http2_connect_content_length_acceptable(stream)) {
            return false;
        }
        // RFC 8441 defines websocket extended CONNECT only for HTTP(S) URI
        // schemes. Other extended protocols retain the full RFC 3986 space.
        if (stream.protocol_is_websocket() &&
            !http_ascii_equals_ignore_case(stream.request_scheme(), "http") &&
            !http_ascii_equals_ignore_case(stream.request_scheme(), "https")) {
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
               (stream.has_authority() && !http2_is_valid_request_authority(
                                              stream.request_scheme(), stream.request_authority())) ||
               !http2_request_path_matches_method_and_scheme(
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
        auto head = http_client_response_head_access::make(*block.status_, http_protocol_version::http2, resource_);
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
            (void)stream.reject_local_connect();
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
    auto scratch = std::move(*push_scratch_);
    push_scratch_.reset();
    std::optional<http2_error_code> refusal;
    if (block.reject_connected_tunnel_) {
        // RFC 9113 §8.5: on a connected stream only DATA, RST_STREAM,
        // WINDOW_UPDATE and PRIORITY are allowed. The promised stream is
        // reserved by the frame (§6.6), so it is cancelled explicitly.
        refusal = http2_error_code::cancel;
    } else if (!finish_request_head(scratch) ||
               (scratch.request_method() != "GET" && scratch.request_method() != "HEAD")) {
        refusal = http2_error_code::protocol_error;
    } else if (const auto* drain = local_connection_state_.graceful_drain();
        drain != nullptr && promised_id > drain->last_stream_id()) {
        refusal = http2_error_code::refused_stream;
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
    http_push_request request(resource_);
    request.method_ = scratch.request_method();
    request.scheme_ = scratch.request_scheme();
    request.authority_ = scratch.request_authority();
    request.path_ = scratch.request_path();
    request.headers_.reserve(scratch.remote_header_count() + static_cast<std::size_t>(scratch.has_cookie()));
    for (std::size_t i = 0; i < scratch.remote_header_count(); ++i) {
        const auto field = scratch.remote_header_at(i);
        request.headers_.emplace_back(field.name_, field.value_, resource_);
    }
    if (scratch.has_cookie()) {
        request.headers_.emplace_back("cookie", scratch.request_cookie(), resource_);
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
            if (const auto priority = priorities_.find(stream.id()); priority != priorities_.end()) {
                stream.set_priority_update(priority->second);
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
    publish_event(http2_event(http2_message_head_record{stream.id(), request_content_signal, body_open}));
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
