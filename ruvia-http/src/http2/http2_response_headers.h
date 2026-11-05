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
#include "ruvia/http/hpack.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_pmr_string.h"
#include "ruvia/http/http_response.h"
#include "ruvia/http/http_response_server.h"

#include "field/http_interim_response_validation.h"
#include "http2/http2_header_list_size.h"
#include "http2/http2_header_rules.h"
#include "http2/http2_hpack.h"
#include "http2/http2_hpack_static_table.h"
#include "http2/http2_response_head_plan.h"
#include "http2/http2_stream_state.h"
#include "response/http_response_header_storage.h"
#include "server/http_date_cache.h"

namespace ruvia::detail {

enum class http2_interim_response_header_encode_status : std::uint8_t { ok,
    invalid_header };

struct http2_known_header_encoding final {
    std::string_view name_;
    std::uint32_t hpack_name_index_{0};
};

// A known name that RFC 7541 Appendix A lists, paired with its static name index.
[[nodiscard]] consteval http2_known_header_encoding http2_static_known_header(std::string_view name) {
    return {.name_ = name, .hpack_name_index_ = hpack_static_name_index(name)};
}

[[nodiscard]] inline http2_known_header_encoding get_http2_known_header_encoding(
    std::uint32_t known_bit) noexcept {
    switch (known_bit) {
        case response_header_content_length:
            return http2_static_known_header("content-length");
        case response_header_content_encoding:
            return http2_static_known_header("content-encoding");
        case response_header_content_type:
            return http2_static_known_header("content-type");
        case response_header_vary:
            return http2_static_known_header("vary");
        case response_header_date:
            return http2_static_known_header("date");
        case response_header_server:
            return http2_static_known_header("server");
        case response_header_cache_control:
            return http2_static_known_header("cache-control");
        case response_header_allow:
            return http2_static_known_header("allow");
        case response_header_access_control_allow_origin:
            return http2_static_known_header("access-control-allow-origin");
        case response_header_access_control_allow_credentials:
            return {.name_ = "access-control-allow-credentials"};
        case response_header_access_control_allow_methods:
            return {.name_ = "access-control-allow-methods"};
        case response_header_access_control_allow_headers:
            return {.name_ = "access-control-allow-headers"};
        case response_header_access_control_max_age:
            return {.name_ = "access-control-max-age"};
        case response_header_access_control_expose_headers:
            return {.name_ = "access-control-expose-headers"};
        case response_header_accept_ranges:
            return http2_static_known_header("accept-ranges");
        case response_header_content_range:
            return http2_static_known_header("content-range");
        case response_header_etag:
            return http2_static_known_header("etag");
        case response_header_last_modified:
            return http2_static_known_header("last-modified");
        case response_header_location:
            return http2_static_known_header("location");
        case response_header_set_cookie:
            return http2_static_known_header("set-cookie");
        default:
            return {};
    }
}

inline void append_http2_encoded_response_header(std::pmr::string& header_block, std::string_view name,
    std::string_view value, std::uint32_t known_bit,
    std::array<char, http2_lower_header_stack_bytes>& lower_name_stack,
    std::pmr::string& lower_name_scratch) {
    const auto known = get_http2_known_header_encoding(known_bit);
    if (known.hpack_name_index_ != 0) {
        hpack_encoder::encode_header_with_name_index(header_block, known.hpack_name_index_, value,
            {.never_indexed_ = hpack_header_name_is_sensitive(known.name_)});
        return;
    }
    hpack_encoder::encode_header(header_block,
        known.name_.empty() ? http2_lower_header_name(name, lower_name_stack, lower_name_scratch)
                            : known.name_,
        value);
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
    std::pmr::string& header_block, const http_interim_response_head& response) {
    if (const auto status = validate_http2_interim_response_headers(response);
        status != http2_interim_response_header_encode_status::ok) {
        return status;
    }
    try {
        header_block.clear();
        const auto headers = response.headers();
        const auto status_token = http_status_code_token(response.status());
        http2_reserve_field_block(header_block,
            http_status_code_token_view(status_token).size() + hpack_literal_overhead_bytes, headers);
        hpack_encoder::encode_status(header_block, response.status());
        std::array<char, http2_lower_header_stack_bytes> lower_name_stack;
        std::pmr::string lower_name_scratch(header_block.get_allocator());
        for (std::size_t i = 0; i < headers.size(); ++i) {
            append_http2_encoded_response_header(header_block, headers[i].name(),
                headers[i].value(), classify_response_header_name(headers[i].name()),
                lower_name_stack, lower_name_scratch);
        }
    } catch (...) {
        header_block.clear();
        throw;
    }
    return http2_interim_response_header_encode_status::ok;
}

[[nodiscard]] inline bool append_http2_response_headers(std::pmr::string& header_block,
    const http_response& response, const http2_response_head_plan& plan) {
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
    std::size_t fixed_bytes = status_value.size() + hpack_literal_overhead_bytes;
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
            fixed_bytes += generated_date.size() + hpack_literal_overhead_bytes;
        }
    }
    std::array<char, http_max_unsigned_decimal_size> content_length_bytes;
    std::string_view content_length_value;
    if (const auto content_length = plan.content_length()) {
        content_length_value = http_format_unsigned(content_length_bytes, *content_length);
        if (!section_size.add("content-length", content_length_value)) {
            return false;
        }
        fixed_bytes += content_length_value.size() + hpack_literal_overhead_bytes;
    }

    try {
        header_block.clear();
        http2_reserve_field_block(header_block, fixed_bytes, response.headers());
        hpack_encoder::encode_status(header_block, plan.body_plan().response_status());
        std::array<char, http2_lower_header_stack_bytes> lower_name_stack;
        std::pmr::string lower_name_scratch(header_block.get_allocator());
        for (const auto& header : response.headers()) {
            const auto known_bit = http_response_header_access::known_bit(header);
            if ((known_bit & writer_framed_bits) != 0) {
                // Content-Length is emitted only from the prepared plan below, so
                // HPACK cannot reinterpret raw application framing independently;
                // the chunked Transfer-Encoding hint has no HTTP/2 form at all.
                continue;
            }
            append_http2_encoded_response_header(header_block, header.name(), header.value(), known_bit,
                lower_name_stack, lower_name_scratch);
        }
        if (!generated_date.empty()) {
            hpack_encoder::encode_header_with_name_index(
                header_block, hpack_static_name_index("date"), generated_date);
        }
        if (!content_length_value.empty()) {
            hpack_encoder::encode_header_with_name_index(
                header_block, hpack_static_name_index("content-length"), content_length_value);
        }
    } catch (...) {
        header_block.clear();
        throw;
    }
    return true;
}

inline void http2_release_local_header_block(std::pmr::string& header_block) {
    http_clear_pmr_string_retaining_small(header_block);
}

inline void append_http2_response_trailers(
    std::pmr::string& trailer_block, const http_response_trailer_section& section) {
    if (section.empty()) {
        return;
    }
    http2_reserve_field_block(trailer_block, 0, section.fields());
    http2_encode_lowercase_fields(trailer_block, section.fields());
}

// The HPACK decode callback for a client-role response trailer section, the
// counterpart of http2_on_decoded_request_trailer. Defined by the connection's
// response-header decoding, which owns what a decoded response may carry.
[[nodiscard]] bool http2_on_decoded_response_trailer(
    void* target, std::string_view name, std::string_view value);

}  // namespace ruvia::detail
