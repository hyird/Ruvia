#include <algorithm>
#include <memory_resource>
#include <utility>

#include "ruvia/http/detail/field/http_header_section_size.h"
#include "ruvia/http/hpack.h"
#include "ruvia/http/http_request_content_semantics.h"

#include "field/http_expect_token.h"
#include "field/http_request_field_validation.h"
#include "http2/http2_connection.h"
#include "http2/http2_header_rules.h"
#include "http2/http2_remote_receive_semantics.h"
#include "http2/http2_request_headers.h"
#include "http2/http2_websocket_handshake.h"
#include "parser/http_request_target.h"
#include "websocket/http_websocket_handshake_fields.h"

// Submitting a request head as the client: what an outbound :method / :path /
// :authority and its header section must satisfy before the connection will
// encode it, for a regular request, a CONNECT tunnel, and the extended CONNECT
// that carries a websocket handshake.

namespace ruvia::detail {
namespace {

[[nodiscard]] bool http2_is_valid_outbound_method(
    std::string_view method, http_known_method known_method) noexcept {
    if (known_method == http_known_method::connect) {
        return false;
    }
    return known_method != http_known_method::unknown || is_valid_http_method_token(method);
}

[[nodiscard]] bool http2_are_valid_outbound_request_headers(std::optional<std::string_view> authority,
    std::uint16_t default_port, std::span<const http_header_view> headers, bool allow_host,
    bool allow_trailers, http_header_section_size& section_size, request_field_state& fields,
    bool websocket_connect = false) noexcept {
    websocket_request_header_validator websocket_headers(http_field_list_role::sender);
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto& header = headers[i];
        if (!http2_is_valid_outbound_regular_header(header.name(), header.value()) ||
            !section_size.add(header.name(), header.value())) {
            return false;
        }
        const auto kind = classify_request_header(header.name());
        if ((!allow_trailers && (kind == request_header_kind::trailer || kind == request_header_kind::te)) ||
            (!allow_host && kind == request_header_kind::host)) {
            return false;
        }
        // Content-Length belongs exclusively to http2_request_content and Expect
        // to http_client_request_expectation; the validator refuses both.
        if (validate_request_field(kind, header.value(), http_field_list_role::sender, fields) !=
            request_field_status::ok) {
            return false;
        }
        if (kind == request_header_kind::host && authority.has_value() &&
            !authority_matches_host(*authority, header.value(), default_port)) {
            return false;
        }
        // Host is already refused for websocket extended CONNECT by the outbound field
        // check (:authority is the sole target authority there).
        if (websocket_connect &&
            (kind == request_header_kind::sec_websocket_key ||
                (kind == request_header_kind::other &&
                    http_ascii_equals_ignore_case(header.name(), "sec-websocket-accept")) ||
                !websocket_headers.observe_header(kind, header.value()))) {
            return false;
        }
    }
    return !websocket_connect || websocket_headers.client_offers_valid();
}

[[nodiscard]] bool http2_is_valid_outbound_regular_request_head(std::string_view method,
    http_known_method known_method, std::string_view scheme, std::uint16_t default_port,
    std::optional<std::string_view> authority, std::string_view path,
    std::span<const http_header_view> headers,
    http_request_content_indication content_indication, std::string_view content_length_value,
    http_client_request_expectation expectation) noexcept {
    if (!http2_is_valid_outbound_method(method, known_method) || !is_valid_uri_scheme(scheme) ||
        (authority.has_value() && !http2_is_valid_request_authority(scheme, *authority)) ||
        !http2_is_valid_regular_request_path(known_method, scheme, path) ||
        (!authority.has_value() && http2_regular_request_requires_authority(scheme, path))) {
        return false;
    }
    // An asterisk-form request target carries no authority information, so a direct
    // HTTP/2 sender MUST NOT generate :authority for it (RFC 9113 Section 8.3.1).
    if (path == "*" && authority.has_value()) {
        return false;
    }
    http_header_section_size section_size;
    bool expect_continue = false;
    switch (expectation) {
        case http_client_request_expectation::none:
            break;
        case http_client_request_expectation::continue_value:
            expect_continue = true;
            break;
        default:
            return false;
    }
    if (!http_client_expectation_is_valid(expect_continue, content_indication) ||
        !section_size.add(":method", method) || !section_size.add(":scheme", scheme) ||
        (authority.has_value() && !section_size.add(":authority", *authority)) ||
        !section_size.add(":path", path) ||
        (expect_continue && !section_size.add("expect", http_continue_expectation_token))) {
        return false;
    }
    request_field_state fields{request_field_framing::binary};
    if (!http2_are_valid_outbound_request_headers(authority, default_port, headers,
            /*allow_host=*/true,
            /*allow_trailers=*/true, section_size, fields)) {
        return false;
    }
    if (!content_length_value.empty() && !section_size.add("content-length", content_length_value)) {
        return false;
    }
    if (content_indication == http_request_content_indication::no_content) {
        return true;
    }
    const auto content_semantics = classify_http_request_content_semantics(method);
    return content_semantics != http_request_content_semantics::forbidden &&
           (content_semantics != http_request_content_semantics::content_type_required ||
               fields.seen(request_header_kind::content_type));
}

void http2_encode_outbound_request_headers(
    std::pmr::string& block, std::span<const http_header_view> headers) {
    std::array<char, http2_lower_header_stack_bytes> lower_name_stack{};
    std::pmr::string lower_name_scratch(block.get_allocator().resource());
    for (const auto& header : headers) {
        hpack_encoder::encode_header(block,
            http2_lower_header_name(header.name(), lower_name_stack, lower_name_scratch),
            header.value());
    }
}

}  // namespace

std::variant<std::uint32_t, http2_request_head_submit_error>
http2_connection::submit_regular_request_head(std::string_view method,
    std::string_view scheme, std::optional<std::string_view> authority, std::string_view path,
    std::span<const http_header_view> headers, http2_request_content content,
    http_client_request_expectation expectation) {
    if (const auto error = local_request_admission_error()) {
        return *error;
    }
    const bool without_content = content.without_content() != nullptr;
    const auto* known_length_content = content.known_length_content();
    const bool streaming_content = content.streaming_content() != nullptr;
    const auto expected_length = known_length_content != nullptr ? std::optional{known_length_content->length()} : streaming_content ? content.streaming_content()->expected_length()
                                                                                                                                     : std::nullopt;
    if (!without_content && known_length_content == nullptr && !streaming_content) {
        return http2_request_head_submit_error::invalid_message;
    }
    const bool content_will_follow = !without_content && (!expected_length || *expected_length != 0);
    const auto content_indication = content_will_follow ? http_request_content_indication::will_follow
                                                        : http_request_content_indication::no_content;
    http2_end_stream end_stream = http2_end_stream::keep_open;
    // Only the formatted prefix is exposed; an omitted length keeps an empty view.
    std::array<char, 20> length_buffer;
    std::size_t length_bytes = 0;
    if (without_content) {
        end_stream = http2_end_stream::end_stream;
    } else if (known_length_content != nullptr) {
        end_stream = known_length_content->length() == 0 ? http2_end_stream::end_stream
                                                         : http2_end_stream::keep_open;
    }
    const auto known_method = classify_http_method(method);
    const auto default_port = http_uri_scheme_default_port(scheme);
    const bool emit_length = expected_length && method != "TRACE";
    if (emit_length) {
        // 20 bytes always hold the canonical decimal form of uint64_t. Do this
        // before mutating stream/HPACK state so every rejection is transactional.
        if (const auto [end, ec] = std::to_chars(length_buffer.data(),
                length_buffer.data() + length_buffer.size(), *expected_length);
            ec == std::errc{}) {
            length_bytes = static_cast<std::size_t>(end - length_buffer.data());
        } else {
            return http2_request_head_submit_error::invalid_message;
        }
    }

    const auto content_length_value = std::string_view(length_buffer.data(), length_bytes);
    // Validate the entire semantic head and decoded-size budget before touching
    // HPACK storage, outbound bytes, stream metadata, or lifecycle state.
    if (!http2_is_valid_outbound_regular_request_head(method, known_method, scheme, default_port,
            authority, path, headers, content_indication, content_length_value, expectation)) {
        return http2_request_head_submit_error::invalid_message;
    }
    return submit_local_request_head([&](http2_stream_state& stream) {
        stream.assign_request_method(method);
        stream.assign_request_scheme(scheme);
        auto& block = stream.local_header_block();
        block.clear();
        hpack_encoder::encode_header(block, ":method", method);
        hpack_encoder::encode_header(block, ":scheme", scheme);
        if (authority.has_value()) {
            hpack_encoder::encode_header(block, ":authority", *authority);
        }
        hpack_encoder::encode_header(block, ":path", path);
        http2_encode_outbound_request_headers(block, headers);
        if (expectation == http_client_request_expectation::continue_value) {
            hpack_encoder::encode_header(block, "expect", http_continue_expectation_token);
        }
        if (emit_length) {
            hpack_encoder::encode_header_with_name_index(block, hpack_static_index::content_length,
                content_length_value);
        }
        // This is the commit point for the wire representation. Every operation
        // above is retryable; append_response_header_frames pre-reserves the complete
        // frame sequence, so a throwing resource cannot strand a created stream.
        append_response_header_frames(stream, std::string_view(block), end_stream);
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
    std::string_view authority, std::span<const http_header_view> headers) {
    if (const auto error = local_request_admission_error()) {
        return *error;
    }

    request_target_view target;
    http_header_section_size section_size;
    request_field_state fields{request_field_framing::binary};
    if (!parse_request_target(http_known_method::connect, authority, target) ||
        !section_size.add(":method", "CONNECT") || !section_size.add(":authority", authority) ||
        !http2_are_valid_outbound_request_headers(authority, 0, headers,
            /*allow_host=*/false,
            /*allow_trailers=*/false, section_size, fields)) {
        return http2_request_head_submit_error::invalid_message;
    }

    return submit_local_request_head([&](http2_stream_state& stream) {
        (void)stream.begin_standard_connect();
        stream.assign_request_method("CONNECT");
        stream.begin_local_content_forbidden();
        (void)stream.begin_local_connect_request();

        auto& block = stream.local_header_block();
        block.clear();
        hpack_encoder::encode_header(block, ":method", "CONNECT");
        hpack_encoder::encode_header(block, ":authority", authority);
        http2_encode_outbound_request_headers(block, headers);
        append_response_header_frames(stream, std::string_view(block), http2_end_stream::keep_open);
    });
}

std::variant<std::uint32_t, http2_request_head_submit_error>
http2_connection::submit_extended_connect_request_head(
    std::string_view protocol, std::string_view scheme, std::string_view authority,
    std::string_view path, std::span<const http_header_view> headers) {
    if (const auto error = local_request_admission_error()) {
        return *error;
    }
    if (!peer_settings_.enable_connect_protocol()) {
        return http2_request_head_submit_error::peer_capability_unavailable;
    }

    const bool websocket_value = http_ascii_equals_ignore_case(protocol, "websocket");
    const auto default_port = http_uri_scheme_default_port(scheme);
    const auto encoded_protocol = websocket_value ? std::string_view("websocket") : protocol;
    http_header_section_size section_size;
    request_field_state fields{request_field_framing::binary};
    if (!is_valid_http_header_name(protocol) || !is_valid_uri_scheme(scheme) ||
        (websocket_value && default_port == 0) || !http2_is_valid_request_authority(scheme, authority) ||
        !http2_is_valid_extended_connect_path(scheme, path) || !section_size.add(":method", "CONNECT") ||
        !section_size.add(":protocol", encoded_protocol) || !section_size.add(":scheme", scheme) ||
        !section_size.add(":authority", authority) || !section_size.add(":path", path) ||
        !http2_are_valid_outbound_request_headers(authority, default_port, headers,
            /*allow_host=*/!websocket_value,
            /*allow_trailers=*/false, section_size, fields,
            /*websocket_connect=*/websocket_value)) {
        return http2_request_head_submit_error::invalid_message;
    }

    return submit_local_request_head([&](http2_stream_state& stream) {
        (void)stream.begin_extended_connect();
        stream.assign_request_method("CONNECT");
        stream.assign_request_scheme(scheme);
        stream.set_protocol(encoded_protocol);
        stream.begin_local_content_forbidden();
        (void)stream.begin_local_connect_request();

        auto& block = stream.local_header_block();
        block.clear();
        hpack_encoder::encode_header(block, ":method", "CONNECT");
        // RFC 8441 registers and requires the lowercase `websocket` value. HTTP
        // protocol-name matching is case-insensitive, so accept caller spelling but
        // never put a non-canonical websocket token on the wire or in stream state.
        hpack_encoder::encode_header(block, ":protocol", encoded_protocol);
        hpack_encoder::encode_header(block, ":scheme", scheme);
        hpack_encoder::encode_header(block, ":authority", authority);
        hpack_encoder::encode_header(block, ":path", path);
        http2_encode_outbound_request_headers(block, headers);
        append_response_header_frames(stream, std::string_view(block), http2_end_stream::keep_open);
    });
}

std::variant<std::uint32_t, http2_push_submit_error> http2_connection::submit_push_promise(
    std::uint32_t associated_stream_id, http_push_request_view request) {
    using error_type = http2_push_submit_error;
    const auto* parent_value = find_stream(associated_stream_id);
    if (role_ != http2_role::server || preface_phase_ != preface_phase_type::ready ||
        local_connection_state_.open() == nullptr || peer_goaway_ || !parent_value || parent_value->is_aborted() ||
        (associated_stream_id & 1U) == 0 || !http2_remote_final_head_decoded(*parent_value) ||
        parent_value->tunnel().not_connect() == nullptr || parent_value->local_send().end_stream_committed() ||
        parent_value->local_send().end_stream_queued() || next_push_stream_id_ > 0x7fffffffU) {
        return error_type::invalid_state;
    }
    if (!peer_settings_.enable_push()) {
        return error_type::push_disabled;
    }
    // RFC 9113 8.4: the client's SETTINGS_MAX_CONCURRENT_STREAMS bounds pushed
    // responses. When no pushed stream could open now (the limit is 0 or already
    // reached by open pushes) the promise is not made. Reserved promises do not
    // count (RFC 9113 5.1.2); their response head waits for a slot instead.
    if (active_local_request_streams_ >= peer_settings_.max_concurrent_streams()) {
        return error_type::stream_limit;
    }
    const auto known_method = request.method_ == "GET"    ? http_known_method::get
                              : request.method_ == "HEAD" ? http_known_method::head
                                                          : http_known_method::unknown;
    const auto default_port = http_uri_scheme_default_port(request.scheme_);
    if (known_method == http_known_method::unknown ||
        !http2_is_valid_outbound_regular_request_head(request.method_, known_method, request.scheme_,
            default_port, request.authority_, request.path_, request.headers_,
            http_request_content_indication::no_content, {}, http_client_request_expectation::none)) {
        return error_type::invalid_request;
    }
    std::pmr::string block(resource_);
    // PUSH_PROMISE begins a field block and therefore carries pending HPACK
    // table-size updates, just like HEADERS.
    if (encoder_table_size_update_pending_) {
        block.append(encoder_zero_table_size_update_);
    }
    hpack_encoder::encode_header(block, ":method", request.method_);
    hpack_encoder::encode_header(block, ":scheme", request.scheme_);
    hpack_encoder::encode_header(block, ":authority", request.authority_);
    hpack_encoder::encode_header(block, ":path", request.path_);
    http2_encode_outbound_request_headers(block, request.headers_);
    auto* stream = create_stream(next_push_stream_id_);
    if (!stream) {
        return error_type::stream_limit;
    }
    const auto checkpoint = output_.checkpoint();
    try {
        stream->assign_request_method(request.method_);
        stream->assign_request_scheme(request.scheme_);
        stream->assign_request_authority(request.authority_);
        stream->assign_request_path(request.path_);
        stream->mark_scheme(default_port);
        stream->mark_authority();
        stream->mark_path();
        std::array<char, http2_lower_header_stack_bytes> lower_name_stack{};
        std::pmr::string lower_name_scratch(resource_);
        for (std::size_t i = 0; i < request.headers_.size(); ++i) {
            const auto& header = request.headers_[i];
            const auto kind = classify_request_header(header.name());
            if (kind == request_header_kind::host) {
                stream->mark_host();
            }
            if (kind == request_header_kind::cookie) {
                if (!http2_append_cookie_header_value(*stream, header.value())) {
                    throw std::length_error("HTTP/2 push request cookie is too large");
                }
            } else {
                const auto lower_name =
                    http2_lower_header_name(header.name(), lower_name_stack, lower_name_scratch);
                if (!stream->append_remote_header(lower_name, header.value(), kind)) {
                    throw std::length_error("HTTP/2 push request has too many fields");
                }
            }
        }
        (void)stream->record_remote_head_end_stream();
        (void)stream->finalize_remote_content_head();
        stream->reserve_push(http2_push_reservation::local);
        std::array<char, 4> promised;
        http2_write32(promised.data(), next_push_stream_id_);
        std::size_t offset = 0;
        bool first = true;
        while (offset < block.size()) {
            const auto prefix = first ? std::string_view(promised.data(), promised.size()) : std::string_view{};
            const auto count = std::min<std::size_t>(block.size() - offset, peer_settings_.max_frame_size() - prefix.size());
            output_.append_frame(first ? http2_frame_type::push_promise : http2_frame_type::continuation,
                offset + count == block.size() ? http2_flag_end_headers : 0, associated_stream_id,
                prefix, std::string_view(block).substr(offset, count));
            first = false;
            offset += count;
        }
    } catch (...) {
        output_.rollback_to(checkpoint);
        streams_.remove(next_push_stream_id_);
        throw;
    }
    encoder_table_size_update_pending_ = false;
    const auto id = next_push_stream_id_;
    next_push_stream_id_ += 2;
    return id;
}

}  // namespace ruvia::detail
