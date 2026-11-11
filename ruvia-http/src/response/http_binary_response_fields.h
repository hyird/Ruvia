#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "ruvia/http/http_interim_response.h"
#include "ruvia/http/http_number_format.h"
#include "ruvia/http/http_response.h"

#include "field/binary_field_section_size.h"
#include "field/http_date_calendar.h"
#include "field/http_interim_response_validation.h"
#include "response/http_framed_response_head_plan.h"
#include "response/http_response_header_bits.h"
#include "response/http_response_header_state.h"
#include "response/http_response_header_storage.h"
#include "websocket/websocket_server_negotiation.h"

namespace ruvia::detail {

// Sinks expose clear(), reserve(count), add(lowercase_name, borrowed_value),
// add_lowercase(name, borrowed_value), and add_owned_value(lowercase_name, value).
// add_owned_value copies temporary formatting/date bytes; the other values must
// outlive submission. Each sink applies http_field_never_indexed to every field.
// Validation and size preflight complete before any sink mutation.
template <typename sink_type>
inline void append_http_binary_response_field(sink_type& fields, std::string_view name,
    std::string_view value, std::uint32_t known_bit) {
    const auto known_name = response_known_header_lowercase_name(known_bit);
    if (known_name.empty()) {
        fields.add_lowercase(name, value);
    } else {
        fields.add(known_name, value);
    }
}

[[nodiscard]] inline bool validate_http_binary_interim_response_fields(
    const http_interim_response_head& response, std::uint64_t limit) noexcept {
    const auto headers = response.headers();
    const auto status_token = http_status_code_token(response.status());
    binary_field_section_size section_size(limit);
    if (!section_size.add(":status", http_status_code_token_view(status_token))) {
        return false;
    }
    http_interim_response_sender_validator validator;
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const auto name = headers[i].name();
        const auto value = headers[i].value();
        if (validator.validate(name, value) != http_interim_response_header_validation_status::ok ||
            is_forbidden_http_binary_response_field(name) || !section_size.add(name, value)) {
            return false;
        }
    }
    return true;
}

template <typename sink_type>
[[nodiscard]] inline bool append_http_binary_interim_response_fields(
    sink_type& fields, const http_interim_response_head& response, std::uint64_t limit) {
    if (!validate_http_binary_interim_response_fields(response, limit)) {
        return false;
    }
    fields.clear();
    const auto headers = response.headers();
    const auto status_token = http_status_code_token(response.status());
    fields.reserve(headers.size() + 1);
    fields.add_owned_value(":status", http_status_code_token_view(status_token));
    for (std::size_t i = 0; i < headers.size(); ++i) {
        append_http_binary_response_field(fields, headers[i].name(), headers[i].value(),
            classify_response_header_name(headers[i].name()));
    }
    return true;
}

template <typename sink_type>
[[nodiscard]] inline bool append_http_binary_response_fields(sink_type& fields,
    const http_response& response, const http_framed_response_head_plan& plan, std::uint64_t limit) {
    // The owning header block already enforces syntax, sender metadata grammar
    // and singleton rules. Only binary framing and section limits remain here.
    const auto known_bits = response_known_header_bits(response);
    const auto status_token = http_status_code_token(plan.body_plan().response_status());
    const auto status_value = http_status_code_token_view(status_token);
    binary_field_section_size section_size(limit);
    if (!section_size.add(":status", status_value)) {
        return false;
    }
    // A replaceable chunked hint has no binary-protocol form; Content-Length
    // comes exclusively from the prepared plan used for content accounting.
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
        if (!generated_date.empty() && !section_size.add("date", generated_date)) {
            return false;
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
            continue;
        }
        append_http_binary_response_field(fields, header.name(), header.value(), known_bit);
    }
    if (!generated_date.empty()) {
        fields.add_owned_value("date", generated_date);
    }
    if (!content_length_value.empty()) {
        fields.add_owned_value("content-length", content_length_value);
    }
    return true;
}

[[nodiscard]] inline bool is_valid_http_binary_connect_response_head(const http_response& response) noexcept {
    const auto& body = response_body(response);
    if (!response.status().is_successful() || body.size() != 0 || body.file().has_value()) {
        return false;
    }
    return !response_has_known_header(
        response, response_header_content_length | response_header_transfer_encoding);
}

template <typename sink_type>
[[nodiscard]] inline bool append_http_binary_websocket_response_fields(
    sink_type& fields, const websocket_server_negotiation& negotiation, std::uint64_t limit) {
    // Negotiation fields are already lowercase and validated by their owner.
    const auto response_fields = negotiation.response_headers();
    const bool has_date = std::ranges::any_of(response_fields, [](const http_header& field) {
        return field.name() == "date";
    });
    const auto date = has_date ? std::string_view{} : cached_date_value();
    const auto subprotocol = negotiation.subprotocol();
    const auto extensions = negotiation.extensions();
    binary_field_section_size section_size(limit);
    if (!section_size.add(":status", "200") ||
        (!date.empty() && !section_size.add("date", date)) ||
        (!subprotocol.empty() && !section_size.add("sec-websocket-protocol", subprotocol)) ||
        (!extensions.empty() && !section_size.add("sec-websocket-extensions", extensions))) {
        return false;
    }
    for (const auto& field : response_fields) {
        if (!section_size.add(field.name(), field.value())) {
            return false;
        }
    }

    fields.clear();
    fields.reserve(response_fields.size() + 4);
    fields.add(":status", "200");
    if (!date.empty()) {
        fields.add_owned_value("date", date);
    }
    if (!subprotocol.empty()) {
        fields.add("sec-websocket-protocol", subprotocol);
    }
    if (!extensions.empty()) {
        fields.add("sec-websocket-extensions", extensions);
    }
    for (const auto& field : response_fields) {
        fields.add(field.name(), field.value());
    }
    return true;
}

}  // namespace ruvia::detail
