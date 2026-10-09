#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ruvia/http/detail/field/header_token_utils.h"
#include "ruvia/http/detail/field/http_header_section_size.h"
#include "ruvia/http/detail/response/http_response_header_state.h"
#include "ruvia/http/detail/server/http_final_response_control_plan.h"
#include "ruvia/http/detail/server/http_response_trailers.h"
#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_response.h"

#include "field/http_interim_response_validation.h"
#include "http2/http2_header_rules.h"
#include "http2/http2_hpack.h"
#include "http2/http2_response_head_plan.h"
#include "http2/http2_stream_state.h"
#include "response/http_response_header_access.h"
#include "server/http_date_cache.h"
#include "util/pmr_string.h"

namespace ruvia::detail {

enum class http2_interim_response_header_encode_status : std::uint8_t { ok,
    invalid_header };

struct http2_known_header_encoding final {
    std::string_view name_;
    std::uint32_t hpack_name_index_{0};
};

inline constexpr std::size_t http2_lower_header_stack_bytes = 64;

[[nodiscard]] inline http2_known_header_encoding get_http2_known_header_encoding(
    std::uint32_t known_bit) noexcept {
    switch (known_bit) {
        case response_header_content_length:
            return {.name_ = "content-length", .hpack_name_index_ = hpack_static_index::content_length};
        case response_header_content_encoding:
            return {
                .name_ = "content-encoding", .hpack_name_index_ = hpack_static_index::content_encoding};
        case response_header_content_type:
            return {.name_ = "content-type", .hpack_name_index_ = hpack_static_index::content_type};
        case response_header_vary:
            return {.name_ = "vary", .hpack_name_index_ = hpack_static_index::vary};
        case response_header_date:
            return {.name_ = "date", .hpack_name_index_ = hpack_static_index::date};
        case response_header_server:
            return {.name_ = "server", .hpack_name_index_ = hpack_static_index::server};
        case response_header_cache_control:
            return {.name_ = "cache-control", .hpack_name_index_ = hpack_static_index::cache_control};
        case response_header_allow:
            return {.name_ = "allow", .hpack_name_index_ = hpack_static_index::allow};
        case response_header_access_control_allow_origin:
            return {.name_ = "access-control-allow-origin",
                .hpack_name_index_ = hpack_static_index::access_control_allow_origin};
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
            return {.name_ = "accept-ranges", .hpack_name_index_ = hpack_static_index::accept_ranges};
        case response_header_content_range:
            return {.name_ = "content-range", .hpack_name_index_ = hpack_static_index::content_range};
        case response_header_etag:
            return {.name_ = "etag", .hpack_name_index_ = hpack_static_index::etag};
        case response_header_last_modified:
            return {.name_ = "last-modified", .hpack_name_index_ = hpack_static_index::last_modified};
        case response_header_location:
            return {.name_ = "location", .hpack_name_index_ = hpack_static_index::location};
        case response_header_set_cookie:
            return {.name_ = "set-cookie", .hpack_name_index_ = hpack_static_index::set_cookie};
        default:
            return {};
    }
}

[[nodiscard]] inline std::string_view http2_lower_header_name(std::string_view name,
    std::array<char, http2_lower_header_stack_bytes>& stack, std::pmr::string& scratch) {
    const auto write_lower = [](std::string_view source_value, char* target) noexcept {
        for (std::size_t i = 0; i < source_value.size(); ++i) {
            target[i] = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(source_value[i])));
        }
    };

    for (const auto ch : name) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 'A' || byte > 'Z') {
            continue;
        }
        if (name.size() <= stack.size()) {
            write_lower(name, stack.data());
            return std::string_view(stack.data(), name.size());
        }
        resize_pmr_string_for_overwrite(scratch, name.size());
        write_lower(name, scratch.data());
        return scratch;
    }
    return name;
}

inline void append_http2_encoded_response_header(std::pmr::string& header_block, std::string_view name,
    std::string_view value, std::uint32_t known_bit,
    std::array<char, http2_lower_header_stack_bytes>& lower_name_stack,
    std::pmr::string& lower_name_scratch) {
    const auto known = get_http2_known_header_encoding(known_bit);
    if (known.hpack_name_index_ != 0) {
        hpack_encoder::encode_header_with_name_index(
            header_block, known.hpack_name_index_, value, hpack_header_name_is_sensitive(known.name_));
        return;
    }
    hpack_encoder::encode_header(header_block,
        known.name_.empty() ? http2_lower_header_name(name, lower_name_stack, lower_name_scratch)
                            : known.name_,
        value);
}

[[nodiscard]] inline bool http2_is_valid_encoded_response_header(
    std::string_view name, std::string_view value, std::uint32_t known_bit) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value) ||
        is_forbidden_http_binary_response_field(name)) {
        return false;
    }
    if (known_bit == response_header_content_type && !is_valid_http_content_type_field_value(value)) {
        return false;
    }
    if (known_bit == response_header_content_encoding &&
        !is_valid_http_content_encoding_field_value(value, http_field_list_role::sender)) {
        return false;
    }
    if (http_ascii_equals_ignore_case(name, "Trailer") &&
        !is_valid_http_response_trailer_field_value(value, http_field_list_role::sender)) {
        return false;
    }
    return true;
}

[[nodiscard]] inline http2_interim_response_header_encode_status validate_http2_interim_response_headers(
    const http_interim_response_head& response) noexcept {
    if (response.headers().size() > max_http_header_fields) {
        return http2_interim_response_header_encode_status::invalid_header;
    }
    const auto common_validation = validate_http_interim_response_headers(response);
    if (common_validation != http_interim_response_header_validation_status::ok) {
        return http2_interim_response_header_encode_status::invalid_header;
    }
    const auto status_token = http_status_code_token(response.status());
    http_header_section_size section_size;
    if (!section_size.add(":status", http_status_code_token_view(status_token))) {
        return http2_interim_response_header_encode_status::invalid_header;
    }
    for (const auto& header : response.headers()) {
        const auto name = header.name();
        // RFC 9113 forbids connection-specific fields in HTTP/2. Common 1xx
        // content/framing and singleton validation has already run above.
        if (is_forbidden_http_binary_response_field(name) ||
            !section_size.add(name, header.value())) {
            return http2_interim_response_header_encode_status::invalid_header;
        }
    }
    return http2_interim_response_header_encode_status::ok;
}

[[nodiscard]] inline http2_interim_response_header_encode_status append_http2_interim_response_headers(
    http2_stream_state& stream, const http_interim_response_head& response) {
    if (const auto status = validate_http2_interim_response_headers(response);
        status != http2_interim_response_header_encode_status::ok) {
        return status;
    }

    auto& header_block = stream.local_header_block();
    try {
        header_block.clear();
        hpack_encoder::encode_status(header_block, response.status());
        std::array<char, http2_lower_header_stack_bytes> lower_name_stack{};
        std::pmr::string lower_name_scratch(header_block.get_allocator());
        for (const auto& header : response.headers()) {
            append_http2_encoded_response_header(header_block, header.name(), header.value(),
                classify_response_header_name(header.name()), lower_name_stack, lower_name_scratch);
        }
    } catch (...) {
        header_block.clear();
        throw;
    }
    return http2_interim_response_header_encode_status::ok;
}

[[nodiscard]] inline bool append_http2_response_headers(http2_stream_state& stream,
    const http_response& response, const http2_response_head_plan& plan,
    const http2_final_response_control& control) {
    // The unforgeable control alternative proves that the same submission path
    // rejected all HTTP/2 connection-specific fields before this function can
    // touch HPACK state. The encoder therefore has no silent filtering branch.
    (void)control;
    const auto known_bits = response_known_header_bits(response);

    const auto status_token = http_status_code_token(plan.body_plan().response_status());
    http_header_section_size section_size;
    if (!section_size.add(":status", http_status_code_token_view(status_token))) {
        return false;
    }
    std::size_t field_count = 0;
    for (const auto& header : response.headers()) {
        const auto known_bit = response_header_known_bit(header);
        if (known_bit == response_header_content_length) {
            continue;
        }
        ++field_count;
        if (field_count > max_http_header_fields ||
            !http2_is_valid_encoded_response_header(header.name(), header.value(), known_bit) ||
            !section_size.add(header.name(), header.value())) {
            return false;
        }
    }
    std::string_view generated_date;
    if ((known_bits & response_header_date) == 0) {
        generated_date = cached_date_value();
        if (!generated_date.empty()) {
            ++field_count;
            if (field_count > max_http_header_fields || !section_size.add("date", generated_date)) {
                return false;
            }
        }
    }
    std::array<char, 20> content_length_bytes{};
    std::string_view content_length_value;
    if (const auto content_length = plan.content_length()) {
        const auto [end, error] = std::to_chars(content_length_bytes.data(),
            content_length_bytes.data() + content_length_bytes.size(), *content_length);
        if (error != std::errc{}) {
            return false;
        }
        content_length_value = std::string_view(
            content_length_bytes.data(), static_cast<std::size_t>(end - content_length_bytes.data()));
        ++field_count;
        if (field_count > max_http_header_fields ||
            !section_size.add("content-length", content_length_value)) {
            return false;
        }
    }

    auto& header_block = stream.local_header_block();
    try {
        header_block.clear();
        hpack_encoder::encode_status(header_block, plan.body_plan().response_status());
        std::array<char, http2_lower_header_stack_bytes> lower_name_stack{};
        std::pmr::string lower_name_scratch(header_block.get_allocator());
        for (const auto& header : response.headers()) {
            const auto known_bit = response_header_known_bit(header);
            if (known_bit == response_header_content_length) {
                // Content-Length is emitted only from the prepared plan below, so
                // HPACK cannot reinterpret raw application framing independently.
                continue;
            }
            append_http2_encoded_response_header(header_block, header.name(), header.value(), known_bit,
                lower_name_stack, lower_name_scratch);
        }
        if (!generated_date.empty()) {
            hpack_encoder::encode_header_with_name_index(
                header_block, hpack_static_index::date, generated_date);
        }
        if (!content_length_value.empty()) {
            hpack_encoder::encode_header_with_name_index(
                header_block, hpack_static_index::content_length, content_length_value);
        }
    } catch (...) {
        header_block.clear();
        throw;
    }
    return true;
}

inline void http2_release_local_header_block(http2_stream_state& stream) {
    clear_pmr_string_retaining_small(stream.local_header_block());
}

inline void append_http2_response_trailers(
    std::pmr::string& trailer_block, const http_response_trailer_section& section) {
    for (const auto& trailer : section.fields()) {
        std::array<char, http2_lower_header_stack_bytes> lower_name_stack{};
        std::pmr::string lower_name_scratch(trailer_block.get_allocator());
        hpack_encoder::encode_header(trailer_block,
            http2_lower_header_name(trailer.name(), lower_name_stack, lower_name_scratch),
            trailer.value());
    }
}

// The HPACK decode callback for a client-role response trailer section, the
// counterpart of http2_on_decoded_request_trailer. Defined by the connection's
// response-header decoding, which owns what a decoded response may carry.
[[nodiscard]] bool http2_on_decoded_response_trailer(
    void* target, std::string_view name, std::string_view value);

}  // namespace ruvia::detail
