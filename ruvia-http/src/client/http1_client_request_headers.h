#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "ruvia/http/detail/field/http_connection_fields.h"
#include "ruvia/http/http1_client_request_writer.h"
#include "ruvia/http/http_header.h"

// What a caller-supplied request header section must satisfy before the writer
// will encode it, and the facts the encoder then needs from it: how many wire
// bytes it costs, which singleton fields it already carries, and the connection
// and upgrade options it expresses.

namespace ruvia {

// The line terminator both the sizing pass and the encoder count in bytes.
inline constexpr std::string_view crlf = "\r\n";

struct request_header_facts final {
    std::size_t wire_bytes_{0};
    std::uint32_t singleton_headers_{0};
    detail::http_connection_options connection_options_;
    detail::http_upgrade_protocols upgrade_protocols_;
    bool has_content_type_{false};
    bool has_te_{false};
};

// Decimal width of a value, for sizing a head before it is filled.
[[nodiscard]] constexpr std::size_t decimal_digits(std::size_t value) noexcept {
    std::size_t digits = 1;
    while (value >= 10) {
        value /= 10;
        ++digits;
    }
    return digits;
}

// Accumulate head bytes, refusing to exceed the header-section ceiling.
[[nodiscard]] bool add_head_bytes(std::size_t& total, std::size_t bytes_value) noexcept;

// A client TE field may only offer trailers or response transfer codings this
// client can decode. "chunked" is never listed because every HTTP/1.1 recipient
// already accepts it as message framing.
[[nodiscard]] bool is_valid_client_te_field(std::string_view value) noexcept;

// Validate every field and collect the facts the encoder needs. Returns false
// with `error` set on the first field that cannot be sent.
[[nodiscard]] bool analyze_headers(std::span<const http_header_view> headers,
    request_header_facts& facts, http1_client_request_prepare_error& error) noexcept;

}  // namespace ruvia
