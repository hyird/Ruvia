#include <algorithm>
#include <memory_resource>
#include <utility>

#include "ruvia/http/detail/field/http_expectations.h"
#include "ruvia/http/detail/field/http_header_section_size.h"
#include "ruvia/http/detail/field/http_trailer_fields.h"
#include "ruvia/http/http_request_content_semantics.h"

#include "coding/http_content_coding.h"
#include "field/http_cors_fields.h"
#include "field/http_media_type.h"
#include "field/http_origin_fields.h"
#include "http2/http2_connection.h"
#include "http2/http2_header_rules.h"
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

struct http2_outbound_request_header_facts final {
    bool has_content_type_{false};
};

[[nodiscard]] bool http2_is_valid_outbound_method(std::string_view method) noexcept {
    return method != "CONNECT" && is_valid_http_method_token(method);
}

[[nodiscard]] bool http2_are_valid_outbound_request_headers(std::optional<std::string_view> authority,
    std::uint16_t default_port, std::span<const http_header_view> headers, bool allow_host,
    bool allow_trailers, http_header_section_size& section_size, std::size_t generated_fields = 0,
    http2_outbound_request_header_facts* facts = nullptr) noexcept {
    if (generated_fields > max_http_header_fields ||
        headers.size() > max_http_header_fields - generated_fields) {
        return false;
    }
    std::uint32_t singleton_headers = 0;
    bool host_seen = false;
    bool has_content_type = false;
    for (const auto& header : headers) {
        if (!http2_is_valid_regular_header(header.name(), header.value()) ||
            !section_size.add(header.name(), header.value())) {
            return false;
        }
        const auto kind = classify_request_header(header.name());
        if ((kind == request_header_kind::origin && !is_valid_http_origin_field_value(header.value())) ||
            (kind == request_header_kind::access_control_request_method &&
                !is_valid_http_cors_request_method(header.value())) ||
            (kind == request_header_kind::access_control_request_headers &&
                !is_valid_http_cors_request_header_names(header.value()))) {
            return false;
        }
        // Content-Length belongs exclusively to http2_request_content, even when a raw
        // value happens to match. Accepting both would restore two framing truths.
        if (kind == request_header_kind::content_length) {
            return false;
        }
        if (header.name() == "trailer") {
            if (!allow_trailers ||
                !is_valid_http_request_trailer_field_value(header.value(), http_field_list_role::sender)) {
                return false;
            }
        } else if (!allow_trailers && header.name() == "te") {
            return false;
        }
        if (kind == request_header_kind::host) {
            if (!allow_host || host_seen || !is_valid_host_header(header.value()) ||
                (authority.has_value() &&
                    !authority_matches_host(*authority, header.value(), default_port))) {
                return false;
            }
            host_seen = true;
        }
        if (kind == request_header_kind::content_type) {
            if (has_content_type || !is_valid_http_content_type_field_value(header.value())) {
                return false;
            }
            has_content_type = true;
        }
        if (kind == request_header_kind::content_encoding &&
            !is_valid_http_content_encoding_field_value(header.value(), http_field_list_role::sender)) {
            return false;
        }
        if (kind == request_header_kind::expect) {
            // Expect is generated only from http_client_request_expectation. Raw
            // fields would create a second source of truth for the body gate.
            return false;
        }
        if (const auto bit = singleton_request_header_bit(kind); bit != 0) {
            if ((singleton_headers & bit) != 0) {
                return false;
            }
            singleton_headers |= bit;
        }
    }
    if (facts != nullptr) {
        facts->has_content_type_ = has_content_type;
    }
    return true;
}

[[nodiscard]] bool http2_is_valid_outbound_regular_request_head(std::string_view method,
    std::string_view scheme, std::optional<std::string_view> authority, std::string_view path,
    std::span<const http_header_view> headers, bool explicit_content,
    http_request_content_indication content_indication, std::string_view content_length_value,
    http_client_request_expectation expectation) noexcept {
    if (!http2_is_valid_outbound_method(method) || !is_valid_uri_scheme(scheme) ||
        (authority.has_value() && !http2_is_valid_request_authority(scheme, *authority)) ||
        !http2_is_valid_regular_request_path(classify_http_method(method), scheme, path) ||
        (!authority.has_value() && http2_regular_request_requires_authority(scheme, path))) {
        return false;
    }
    const auto default_port = http_uri_scheme_default_port(scheme);
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
    http2_outbound_request_header_facts facts;
    if (!http2_are_valid_outbound_request_headers(authority, default_port, headers,
            /*allow_host=*/true,
            /*allow_trailers=*/true, section_size,
            (content_length_value.empty() ? 0 : 1) + (expect_continue ? 1 : 0), &facts)) {
        return false;
    }
    if (!content_length_value.empty() && !section_size.add("content-length", content_length_value)) {
        return false;
    }
    if (!explicit_content) {
        return true;
    }
    const auto content_semantics = http_request_content_semantics(method);
    return content_semantics != http_request_content_semantics::forbidden &&
           (content_semantics != http_request_content_semantics::content_type_required ||
               facts.has_content_type_);
}

[[nodiscard]] bool http2_is_valid_websocket_connect_headers(
    std::span<const http_header_view> headers) noexcept {
    bool saw_version = false;
    for (const auto& header : headers) {
        if (header.name() == "host" || header.name() == "sec-websocket-key" ||
            header.name() == "sec-websocket-accept") {
            return false;
        }
        if (header.name() == "sec-websocket-version") {
            if (saw_version || header.value() != "13") {
                return false;
            }
            saw_version = true;
        }
    }
    return saw_version && websocket_client_offer_headers_valid(headers);
}

void http2_encode_outbound_request_headers(
    std::pmr::string& block, std::span<const http_header_view> headers) {
    for (const auto& header : headers) {
        hpack_encoder::encode_header(block, header.name(), header.value());
    }
}

}  // namespace

http2_request_head_submit_result http2_connection::submit_regular_request_head(std::string_view method,
    std::string_view scheme, std::optional<std::string_view> authority, std::string_view path,
    std::span<const http_header_view> headers, http2_request_content content,
    http_client_request_expectation expectation) {
    if (const auto error = local_request_admission_error()) {
        return http2_request_head_submit_result::make_failure(*error);
    }
    const bool without_content = content.without_content() != nullptr;
    const auto* known_length_content = content.known_length_content();
    const bool streaming_content = content.streaming_content() != nullptr;
    const auto expected_length = known_length_content != nullptr ? std::optional{known_length_content->length()} : streaming_content ? content.streaming_content()->expected_length()
                                                                                                                                     : std::nullopt;
    if (!without_content && known_length_content == nullptr && !streaming_content) {
        return http2_request_head_submit_result::make_failure(
            http2_request_head_submit_error::invalid_message);
    }
    const bool content_will_follow =
        streaming_content || (known_length_content != nullptr && known_length_content->length() != 0);
    const auto content_indication = content_will_follow ? http_request_content_indication::will_follow
                                                        : http_request_content_indication::no_content;
    http2_end_stream end_stream = http2_end_stream::keep_open;
    std::array<char, 20> length_buffer{};
    std::size_t length_bytes = 0;
    if (without_content) {
        end_stream = http2_end_stream::end_stream;
    } else if (known_length_content != nullptr) {
        end_stream = known_length_content->length() == 0 ? http2_end_stream::end_stream
                                                         : http2_end_stream::keep_open;
    }
    if (expected_length) {
        // 20 bytes always hold the canonical decimal form of uint64_t. Do this
        // before mutating stream/HPACK state so every rejection is transactional.
        if (const auto [end, ec] = std::to_chars(length_buffer.data(),
                length_buffer.data() + length_buffer.size(), *expected_length);
            ec == std::errc{}) {
            length_bytes = static_cast<std::size_t>(end - length_buffer.data());
        } else {
            return http2_request_head_submit_result::make_failure(
                http2_request_head_submit_error::invalid_message);
        }
    }

    const auto content_length_value = std::string_view(length_buffer.data(), length_bytes);
    // Validate the entire semantic head and decoded-size budget before touching
    // HPACK storage, outbound bytes, stream metadata, or lifecycle state.
    if (!http2_is_valid_outbound_regular_request_head(method, scheme, authority, path, headers,
            !without_content, content_indication, content_length_value, expectation)) {
        return http2_request_head_submit_result::make_failure(
            http2_request_head_submit_error::invalid_message);
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
        if (expected_length) {
            hpack_encoder::encode_header_with_name_index(block, hpack_static_index::content_length,
                std::string_view(length_buffer.data(), length_bytes));
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

http2_request_head_submit_result http2_connection::submit_connect_request_head(
    std::string_view authority, std::span<const http_header_view> headers) {
    if (const auto error = local_request_admission_error()) {
        return http2_request_head_submit_result::make_failure(*error);
    }

    request_target_view target;
    http_header_section_size section_size;
    if (!parse_request_target(http_known_method::connect, authority, target) ||
        !section_size.add(":method", "CONNECT") || !section_size.add(":authority", authority) ||
        !http2_are_valid_outbound_request_headers(authority, 0, headers,
            /*allow_host=*/false,
            /*allow_trailers=*/false, section_size)) {
        return http2_request_head_submit_result::make_failure(
            http2_request_head_submit_error::invalid_message);
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

http2_request_head_submit_result http2_connection::submit_extended_connect_request_head(
    std::string_view protocol, std::string_view scheme, std::string_view authority,
    std::string_view path, std::span<const http_header_view> headers) {
    if (const auto error = local_request_admission_error()) {
        return http2_request_head_submit_result::make_failure(*error);
    }
    if (!peer_settings_.enable_connect_protocol()) {
        return http2_request_head_submit_result::make_failure(
            http2_request_head_submit_error::peer_capability_unavailable);
    }

    const bool websocket_value = http_ascii_equals_ignore_case(protocol, "websocket");
    const bool websocket_scheme_value =
        http_ascii_equals_ignore_case(scheme, "http") || http_ascii_equals_ignore_case(scheme, "https");
    const auto encoded_protocol = websocket_value ? std::string_view("websocket") : protocol;
    http_header_section_size section_size;
    if (!is_valid_http_header_name(protocol) || !is_valid_uri_scheme(scheme) ||
        (websocket_value && !websocket_scheme_value) || !http2_is_valid_request_authority(scheme, authority) ||
        !http2_is_valid_extended_connect_path(scheme, path) || !section_size.add(":method", "CONNECT") ||
        !section_size.add(":protocol", encoded_protocol) || !section_size.add(":scheme", scheme) ||
        !section_size.add(":authority", authority) || !section_size.add(":path", path) ||
        !http2_are_valid_outbound_request_headers(authority, http_uri_scheme_default_port(scheme), headers,
            /*allow_host=*/!websocket_value,
            /*allow_trailers=*/false, section_size) ||
        (websocket_value && !http2_is_valid_websocket_connect_headers(headers))) {
        return http2_request_head_submit_result::make_failure(
            http2_request_head_submit_error::invalid_message);
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
        (associated_stream_id & 1U) == 0 || parent_value->local_send().end_stream_committed() ||
        parent_value->local_send().end_stream_queued() || next_push_stream_id_ > 0x7fffffffU) {
        return error_type::invalid_state;
    }
    if (!peer_settings_.enable_push()) {
        return error_type::push_disabled;
    }
    if ((request.method_ != "GET" && request.method_ != "HEAD") ||
        !http2_is_valid_outbound_regular_request_head(request.method_, request.scheme_, request.authority_, request.path_,
            request.headers_, false, http_request_content_indication::no_content, {}, http_client_request_expectation::none)) {
        return error_type::invalid_request;
    }
    std::pmr::string block(resource_);
    // PUSH_PROMISE begins a field block and therefore carries pending HPACK
    // table-size updates, just like HEADERS.
    if (encoder_table_size_update_pending_) {
        hpack_encoder::encode_dynamic_table_size_update(block, encoder_dynamic_table_size_);
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
        stream->mark_scheme(http_uri_scheme_default_port(request.scheme_));
        stream->mark_authority();
        stream->mark_path();
        for (const auto& header : request.headers_) {
            const auto kind = classify_request_header(header.name());
            if (kind == request_header_kind::host) {
                stream->mark_host();
            }
            if (kind == request_header_kind::cookie) {
                if (!http2_append_cookie_header_value(*stream, header.value())) {
                    throw std::length_error("HTTP/2 push request cookie is too large");
                }
            } else if (!stream->append_remote_header(header.name(), header.value(), kind)) {
                throw std::length_error("HTTP/2 push request has too many fields");
            }
        }
        (void)stream->record_remote_head_end_stream();
        (void)stream->finalize_remote_content_head();
        stream->reserve_push(http2_push_reservation::local);
        std::array<char, 4> promised{};
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
