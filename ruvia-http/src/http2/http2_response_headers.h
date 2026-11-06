#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/detail/field/http_field_list_grammar.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/detail/server/http_final_response_control_plan.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "field/http_interim_response_validation.h"
#include "http2/http2_field_list.h"
#include "http2/http2_header_list_size.h"
#include "http2/http2_header_rules.h"
#include "http2/http2_stream_state.h"
#include "response/http_framed_response_head_plan.h"
#include "response/http_response_header_storage.h"
#include "server/http_date_cache.h"

namespace ruvia::detail {

enum class http2_interim_response_header_encode_status : std::uint8_t { ok,
    invalid_header };

[[nodiscard]] inline std::string_view get_http2_known_header_name(
    std::uint32_t known_bit) noexcept {
    switch (known_bit) {
        case response_header_content_length:
            return "content-length";
        case response_header_content_encoding:
            return "content-encoding";
        case response_header_content_type:
            return "content-type";
        case response_header_vary:
            return "vary";
        case response_header_date:
            return "date";
        case response_header_server:
            return "server";
        case response_header_cache_control:
            return "cache-control";
        case response_header_allow:
            return "allow";
        case response_header_access_control_allow_origin:
            return "access-control-allow-origin";
        case response_header_access_control_allow_credentials:
            return "access-control-allow-credentials";
        case response_header_access_control_allow_methods:
            return "access-control-allow-methods";
        case response_header_access_control_allow_headers:
            return "access-control-allow-headers";
        case response_header_access_control_max_age:
            return "access-control-max-age";
        case response_header_access_control_expose_headers:
            return "access-control-expose-headers";
        case response_header_accept_ranges:
            return "accept-ranges";
        case response_header_content_range:
            return "content-range";
        case response_header_etag:
            return "etag";
        case response_header_last_modified:
            return "last-modified";
        case response_header_location:
            return "location";
        case response_header_set_cookie:
            return "set-cookie";
        default:
            return {};
    }
}

inline void append_http2_response_field(http2_field_list& fields, std::string_view name,
    std::string_view value, std::uint32_t known_bit) {
    const auto known_name = get_http2_known_header_name(known_bit);
    if (known_name.empty()) {
        fields.add_lowercase(name, value);
    } else {
        fields.add(known_name, value);
    }
}

[[nodiscard]] inline http2_interim_response_header_encode_status validate_http2_interim_response_headers(
    const http_interim_response_head& response) noexcept {
    const auto headers = response.headers();
    const auto status_token = http_status_code_token(response.status());
    http2_header_list_size section_size;
    if (!section_size.add(":status", http_status_code_token_view(status_token))) {
        return http2_interim_response_header_encode_status::invalid_header;
    }
    http_interim_response_sender_validator validator;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto name = headers[i].name();
        const auto value = headers[i].value();
        if (validator.validate(name, value) !=
                http_interim_response_header_validation_status::ok ||
            is_forbidden_http_binary_response_field(name) ||
            !section_size.add(name, value)) {
            return http2_interim_response_header_encode_status::invalid_header;
        }
    }
    return http2_interim_response_header_encode_status::ok;
}

[[nodiscard]] inline http2_interim_response_header_encode_status append_http2_interim_response_headers(
    http2_field_list& fields, const http_interim_response_head& response) {
    if (const auto status = validate_http2_interim_response_headers(response);
        status != http2_interim_response_header_encode_status::ok) {
        return status;
    }
    fields.clear();
    const auto headers = response.headers();
    const auto status_token = http_status_code_token(response.status());
    fields.reserve(headers.size() + 1);
    fields.add_owned_value(":status", http_status_code_token_view(status_token));
    for (std::size_t i = 0; i < headers.size(); ++i) {
        append_http2_response_field(fields, headers[i].name(), headers[i].value(),
            classify_response_header_name(headers[i].name()));
    }
    return http2_interim_response_header_encode_status::ok;
}

[[nodiscard]] inline bool append_http2_response_headers(http2_field_list& fields,
    const http_response& response, const http_framed_response_head_plan& plan) {
    // The owning header block already enforces field syntax, sender metadata
    // grammar and singleton rules. Revisit only protocol-specific section limits,
    // not the same field bytes or metadata parsers.
    const auto known_bits = response_known_header_bits(response);

    const auto status_token = http_status_code_token(plan.body_plan().response_status());
    const auto status_value = http_status_code_token_view(status_token);
    http2_header_list_size section_size;
    if (!section_size.add(":status", status_value)) {
        return false;
    }
    // http2_final_response_control_plan admitted a Transfer-Encoding only as the
    // replaceable chunked hint, which HTTP/2 framing supersedes.
    constexpr auto writer_framed_bits = response_header_content_length | response_header_transfer_encoding;
    for (const auto& header : response.headers()) {
        if ((http_response_header_access::known_bit(header) & writer_framed_bits) != 0) {
            continue;
        }
        if (!section_size.add(header.name(), header.value())) {
            return false;
        }
    }
    std::string_view generated_date;
    if ((known_bits & response_header_date) == 0) {
        generated_date = cached_date_value();
        if (!generated_date.empty()) {
            if (!section_size.add("date", generated_date)) {
                return false;
            }
        }
    }
    std::array<char, http_max_unsigned_decimal_size> content_length_bytes;
    std::string_view content_length_value;
    if (const auto content_length = plan.content_length()) {
        content_length_value = http_format_unsigned(content_length_bytes, *content_length);
        if (!section_size.add("content-length", content_length_value)) {
            return false;
        }
    }

    fields.clear();
    fields.reserve(response.headers().size() + 3);
    fields.add_owned_value(":status", status_value);
    for (const auto& header : response.headers()) {
        const auto known_bit = http_response_header_access::known_bit(header);
        if ((known_bit & writer_framed_bits) != 0) {
            // Content-Length is emitted only from the prepared plan below, so
            // HPACK cannot reinterpret raw application framing independently;
            // the chunked Transfer-Encoding hint has no HTTP/2 form at all.
            continue;
        }
        append_http2_response_field(fields, header.name(), header.value(), known_bit);
    }
    if (!generated_date.empty()) {
        fields.add_owned_value("date", generated_date);
    }
    if (!content_length_value.empty()) {
        fields.add_owned_value("content-length", content_length_value);
    }
    return true;
}

inline void append_http2_response_trailers(
    http2_field_list& fields, const http_response_trailer_section& section) {
    fields.clear();
    fields.reserve(section.fields().size());
    fields.add_lowercase_fields(section.fields());
}

// The HPACK decode callback for a client-role response trailer section, the
// counterpart of http2_on_decoded_request_trailer. Defined by the connection's
// response-header decoding, which owns what a decoded response may carry.
[[nodiscard]] bool http2_on_decoded_response_trailer(
    void* target, std::string_view name, std::string_view value);

}  // namespace ruvia::detail
