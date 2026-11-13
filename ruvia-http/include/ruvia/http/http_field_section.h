#pragma once

#include <cstddef>
#include <limits>
#include <span>
#include <string_view>

#include "ruvia/http/http_header.h"
#include "ruvia/http/http_limits.h"
#include "ruvia/http/http_status.h"

namespace ruvia {

class http_response_headers;

// Bytes a field costs beyond its name and value: the ": " separator and the
// CRLF of its normalized HTTP/1 field line "name: value\r\n".
inline constexpr std::size_t http_field_line_overhead = 4;

// The byte and count rule for an HTTP field section, for headers and trailers,
// requests and responses, received and generated: the section must fit
// max_http_header_bytes, each field costing name.size() + value.size() +
// http_field_line_overhead (independent of received optional whitespace), and
// hold at most max_http_header_fields fields.
// HTTP/2 header lists are the deliberate exception: they are charged by the
// RFC 9113 section 6.5.2 SETTINGS_MAX_HEADER_LIST_SIZE accounting (name and
// value plus 32 bytes per field, pseudo-fields included) against the same limit.
// Fixed head bytes such as a start line or the terminating empty line are
// charged with add(); a trailer section starts from
// http_trailer_section_budget(). A failed charge leaves the budget unchanged.
class http_field_section_budget final {
public:
    [[nodiscard]] constexpr bool add(std::size_t bytes_value) noexcept {
        if (bytes_value > max_http_header_bytes - bytes_) {
            return false;
        }
        bytes_ += bytes_value;
        return true;
    }

    [[nodiscard]] constexpr bool add_field(std::size_t name_bytes, std::size_t value_bytes) noexcept {
        const auto remaining = max_http_header_bytes - bytes_;
        if (fields_ == max_http_header_fields || remaining < http_field_line_overhead ||
            name_bytes > remaining - http_field_line_overhead ||
            value_bytes > remaining - http_field_line_overhead - name_bytes) {
            return false;
        }
        bytes_ += name_bytes + value_bytes + http_field_line_overhead;
        ++fields_;
        return true;
    }

    [[nodiscard]] constexpr bool add_field(std::string_view name, std::string_view value) noexcept {
        return add_field(name.size(), value.size());
    }

    [[nodiscard]] constexpr std::size_t bytes() const noexcept {
        return bytes_;
    }

private:
    std::size_t bytes_{0};
    std::size_t fields_{0};
};

// The budget of a trailer section before its first field. Like a head, the
// section is charged its terminating empty line, so every section a writer
// generates also fits the decoders' limit on the complete encoded section.
[[nodiscard]] constexpr http_field_section_budget http_trailer_section_budget() noexcept {
    http_field_section_budget budget;
    (void)budget.add(std::string_view("\r\n").size());
    return budget;
}

namespace detail {

[[nodiscard]] constexpr std::size_t saturating_add_size(
    std::size_t left, std::size_t right) noexcept {
    constexpr auto maximum = (std::numeric_limits<std::size_t>::max)();
    return left > maximum - right ? maximum : left + right;
}

template <typename field_span_type>
[[nodiscard]] constexpr std::size_t estimate_http1_fields_bytes_impl(
    const field_span_type& fields, bool include_terminating_crlf) noexcept {
    std::size_t total = include_terminating_crlf ? 2U : 0U;
    for (const auto& field : fields) {
        total = saturating_add_size(total, field.name().size());
        total = saturating_add_size(total, field.value().size());
        total = saturating_add_size(total, http_field_line_overhead);
    }
    return total;
}

}  // namespace detail

[[nodiscard]] constexpr std::size_t estimate_http1_field_section_bytes(
    std::span<const http_header> fields, bool include_terminating_crlf = true) noexcept {
    return detail::estimate_http1_fields_bytes_impl(fields, include_terminating_crlf);
}

[[nodiscard]] constexpr std::size_t estimate_http1_field_section_bytes(
    std::span<const http_header_view> fields, bool include_terminating_crlf = true) noexcept {
    return detail::estimate_http1_fields_bytes_impl(fields, include_terminating_crlf);
}

[[nodiscard]] std::size_t estimate_http1_field_section_bytes(
    const http_response_headers& fields, bool include_terminating_crlf = true) noexcept;

[[nodiscard]] constexpr std::size_t estimate_http1_trailers_bytes(
    std::span<const http_header> trailers, bool include_terminating_crlf = true) noexcept {
    return estimate_http1_field_section_bytes(trailers, include_terminating_crlf);
}

[[nodiscard]] constexpr std::size_t estimate_http1_trailers_bytes(
    std::span<const http_header_view> trailers, bool include_terminating_crlf = true) noexcept {
    return estimate_http1_field_section_bytes(trailers, include_terminating_crlf);
}

// Equivalent HTTP/1.1 wire byte size of a response status line ("HTTP/1.1 NNN
// <reason>\r\n"), all header field lines, and the terminating CRLF.
[[nodiscard]] constexpr std::size_t estimate_http1_response_head_bytes(
    http_status_code status, std::span<const http_header> headers) noexcept {
    const std::size_t status_line_bytes = 15U + http_reason_phrase(status).size();
    return detail::saturating_add_size(
        status_line_bytes, estimate_http1_field_section_bytes(headers, true));
}

[[nodiscard]] constexpr std::size_t estimate_http1_response_head_bytes(
    http_status_code status, std::span<const http_header_view> headers) noexcept {
    const std::size_t status_line_bytes = 15U + http_reason_phrase(status).size();
    return detail::saturating_add_size(
        status_line_bytes, estimate_http1_field_section_bytes(headers, true));
}

[[nodiscard]] std::size_t estimate_http1_response_head_bytes(
    http_status_code status, const http_response_headers& headers) noexcept;

}  // namespace ruvia
