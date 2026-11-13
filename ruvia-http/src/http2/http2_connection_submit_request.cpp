#include <algorithm>
#include <array>
#include <string>
#include <utility>

#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_request_content_semantics.h"

#include "field/http_lower_header_name.h"
#include "field/http_request_field_validation.h"
#include "http2/http2_connection.h"
#include "parser/http_request_target.h"
#include "request/http_binary_request_fields.h"
#include "websocket/http_websocket_handshake_fields.h"

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
