#pragma once

#include <span>
#include <string_view>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_header.h"

#include "field/http_request_field_validation.h"

// What a caller-supplied request header section must satisfy before the writer
// will encode it, and the facts the encoder then needs from it: its field
// section budget, which singleton fields it already carries, and the
// connection and upgrade options it expresses.

namespace ruvia::detail {

struct http1_client_request_field_facts final {
    // The caller's fields, charged by the shared field-section rule.
    http_field_section_budget section_;
    detail::request_field_state fields_{detail::request_field_framing::http1};
    detail::http_connection_options connection_options_;
    detail::http_upgrade_protocols upgrade_protocols_;
    std::string_view first_upgrade_value_{};
    std::size_t upgrade_value_bytes_{0};
    std::size_t upgrade_field_count_{0};
    bool has_te_{false};
    // The caller declared trailer fields; the writer requires chunked content.
    bool has_trailer_{false};
};

[[nodiscard]] bool analyze_http1_client_request_field(http_header_view header, detail::request_header_kind kind,
    http1_client_request_field_facts& facts, http1_client_request_prepare_error& error,
    http_request_field_source source = http_request_field_source::generated) noexcept;

[[nodiscard]] bool finish_http1_client_request_fields(
    const http1_client_request_field_facts& facts, http1_client_request_prepare_error& error) noexcept;

// Validate every field and collect the facts the encoder needs. Returns false
// with `error` set on the first field that cannot be sent.
[[nodiscard]] bool analyze_http1_client_request_fields(std::span<const http_header_view> headers,
    http1_client_request_field_facts& facts, http1_client_request_prepare_error& error,
    http_request_field_source source) noexcept;

}  // namespace ruvia::detail
