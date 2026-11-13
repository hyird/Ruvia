#include "ruvia/http/http_field_section.h"
#include "ruvia/http/http_field_values.h"
#pragma once

#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "ruvia/http/http_header.h"

namespace ruvia {
enum class http_request_trailer_error : unsigned char { invalid_field,
    forbidden_field,
    section_too_large };

// Owns decoded terminal request fields, separately from the initial head.
// Drivers populate this object before reporting successful body completion.
// The section must fit the http_field_section_budget byte and field-count rule,
// starting from http_trailer_section_budget(). Retained field names are
// lowercase whichever append overload or protocol supplied them.
class http_request_trailers final {
public:
    http_request_trailers() noexcept = default;
    http_request_trailers(const http_request_trailers&) = delete;
    http_request_trailers& operator=(const http_request_trailers&) = delete;
    // Appends one field or a span of fields all-or-nothing. A name not permitted
    // in request trailers (RFC 9110 section 6.5.1) reports forbidden_field.
    [[nodiscard]] std::optional<http_request_trailer_error> append(std::string_view name, std::string_view value);
    [[nodiscard]] std::optional<http_request_trailer_error> append(std::span<const http_header_view> fields);
    template <detail::http_temporary_owning_range<http_header_view> fields_type>
    std::optional<http_request_trailer_error> append(fields_type&&) = delete;
    [[nodiscard]] std::optional<http_request_trailer_error> append(std::span<const http_header> fields);
    template <detail::http_temporary_owning_range<http_header> fields_type>
    std::optional<http_request_trailer_error> append(fields_type&&) = delete;
    // Appends a received trailer section all-or-nothing, as decoded by any
    // protocol version (http1_chunked_body_decoder, HTTP/2 or HTTP/3 message
    // end events). Every field must be a valid regular field with a lowercase
    // name; a pseudo-header field makes the section malformed (RFC 9113
    // section 8.1, RFC 9114 section 4.3) and reports invalid_field. Every
    // received field is charged to the byte budget; fields not permitted in
    // request trailers are discarded rather than retained (RFC 9110 section
    // 6.5.1, RFC 9112 section 7.1.2).
    [[nodiscard]] std::optional<http_request_trailer_error> append_received(std::span<const http_header> fields);
    template <detail::http_temporary_owning_range<http_header> fields_type>
    std::optional<http_request_trailer_error> append_received(fields_type&&) = delete;
    [[nodiscard]] std::vector<http_header> take_fields() && noexcept {
        budget_ = http_trailer_section_budget();
        return std::move(fields_);
    }
    [[nodiscard]] std::span<const http_header> fields() const& noexcept RUVIA_LIFETIMEBOUND {
        return fields_;
    }
    std::span<const http_header> fields() const&& = delete;
    [[nodiscard]] std::optional<std::string_view> field(std::string_view name) const& noexcept RUVIA_LIFETIMEBOUND;
    std::optional<std::string_view> field(std::string_view) const&& = delete;

private:
    std::vector<http_header> fields_;
    http_field_section_budget budget_{http_trailer_section_budget()};
};
}  // namespace ruvia

namespace ruvia::detail {

enum class http_field_kind : std::uint8_t;

// Shared request/response restrictions (RFC 9110 section 6.5.1): fields that
// control framing, routing, authentication or representation interpretation.
// Direction-specific permissions are composed below and by response policy.
[[nodiscard]] bool is_forbidden_common_trailer_name(
    std::string_view name, http_field_kind kind) noexcept;

// Trailer advertisements and actual request sections use the same policy.
// Accept-Ranges is permitted in response trailers, not request trailers.
[[nodiscard]] bool is_forbidden_http_request_trailer_name(std::string_view name) noexcept;

template <typename forbidden_name_type>
[[nodiscard]] inline bool is_valid_http_trailer_field_value(
    std::string_view value, http_field_list_role role, forbidden_name_type&& forbidden_name) noexcept {
    // RFC 9110 section 5.6.1.1: senders must not generate empty list elements,
    // but `#field-name` itself can represent an empty list.
    const auto list_role = http_trim_ows(value).empty() ? http_field_list_role::recipient : role;
    return http_visit_token_list(value, list_role, [&forbidden_name](std::string_view name) noexcept {
        return !forbidden_name(name);
    }) == http_field_list_parse_status::ok;
}

[[nodiscard]] inline bool is_valid_http_request_trailer_field_value(
    std::string_view value, http_field_list_role role) noexcept {
    return is_valid_http_trailer_field_value(value, role,
        [](std::string_view name) noexcept { return is_forbidden_http_request_trailer_name(name); });
}

// The single rule for one generated request trailer field: valid field syntax
// (RFC 9110 section 5), a name a sender may place in request trailers (RFC 9110
// section 6.5.1), and a charge to the section `budget`, which is unchanged on
// failure.
[[nodiscard]] inline std::optional<http_request_trailer_error> charge_http_request_trailer_field(
    http_field_section_budget& budget, std::string_view name, std::string_view value) noexcept {
    if (!is_valid_http_header_name(name) || !is_valid_http_header_value(value)) {
        return http_request_trailer_error::invalid_field;
    }
    if (is_forbidden_http_request_trailer_name(name)) {
        return http_request_trailer_error::forbidden_field;
    }
    if (!budget.add_field(name, value)) {
        return http_request_trailer_error::section_too_large;
    }
    return std::nullopt;
}

}  // namespace ruvia::detail
