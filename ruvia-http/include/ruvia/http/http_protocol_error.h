#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "ruvia/http/http_status.h"

namespace ruvia {

inline constexpr std::size_t max_http_header_bytes = std::size_t{64} * 1024;
// Field lines per HTTP/1 head or trailer section (the http_field_section_budget
// rule), received and generated, beside the max_http_header_bytes byte cap. The
// smallest field line costs 4 bytes, so the byte cap alone admits ~16 K fields
// whose descriptors outweigh the head; 256 matches Pingora's parser limit.
inline constexpr std::size_t max_http_header_fields = 256;
inline constexpr std::size_t default_max_buffered_body_bytes = std::size_t{16} * 1024 * 1024;
inline constexpr std::size_t default_max_websocket_message_bytes = std::size_t{16} * 1024 * 1024;
// Bytes one SSE event may buffer while sse_parser assembles it.
inline constexpr std::size_t default_max_sse_event_bytes = std::size_t{16} * 1024 * 1024;

}  // namespace ruvia

namespace ruvia {

// A protocol byte ceiling with no numeric sentinel: the default is unlimited,
// while a configured limit is always strictly positive.
class protocol_byte_limit final {
public:
    protocol_byte_limit() noexcept = default;

    [[nodiscard]] static protocol_byte_limit unlimited() noexcept {
        return protocol_byte_limit();
    }

    // Validate the original integer before converting to the stored size.
    // Negative inputs must not become effectively unbounded positive ceilings.
    template <std::integral integer_type>
    [[nodiscard]] static protocol_byte_limit limited(integer_type bytes_value) {
        if (!std::in_range<std::size_t>(+bytes_value) || +bytes_value == 0) {
            throw std::invalid_argument("protocol byte limit must be positive and representable");
        }
        return protocol_byte_limit(static_cast<std::size_t>(bytes_value));
    }

    [[nodiscard]] bool is_limited() const noexcept {
        return maximum_.has_value();
    }

    [[nodiscard]] std::optional<std::size_t> maximum() const noexcept {
        return maximum_;
    }

    [[nodiscard]] bool exceeds(std::size_t bytes_value) const noexcept {
        return maximum_.has_value() && bytes_value > *maximum_;
    }

    [[nodiscard]] bool addition_exceeds(std::size_t current, std::size_t added) const noexcept {
        if (added > (std::numeric_limits<std::size_t>::max)() - current) {
            return true;
        }
        return maximum_.has_value() && (current > *maximum_ || added > *maximum_ - current);
    }

    [[nodiscard]] std::size_t read_ceiling() const noexcept {
        return maximum_.value_or((std::numeric_limits<std::size_t>::max)());
    }

private:
    explicit protocol_byte_limit(std::size_t bytes_value) noexcept
        : maximum_(bytes_value) {}

    std::optional<std::size_t> maximum_;
};

}  // namespace ruvia

namespace ruvia {

// An HTTP protocol primitive rejected input with a response status that a
// server driver may map onto its own application error model. `code` is a
// stable snake_case machine token naming the specific failure (for example
// "missing_host") and must have static storage duration: it is kept as a view.
// The human-readable message is copied into fixed exception storage, so
// reporting never allocates or borrows.
class http_protocol_error final : public std::exception {
public:
    http_protocol_error(
        http_status_code status, std::string_view code, std::string_view message) noexcept
        : status_(status),
          code_(code) {
        const auto size = (std::min)(message.size(), message_.size() - 1);
        if (size != 0) {
            std::memcpy(message_.data(), message.data(), size);
        }
        message_[size] = '\0';
    }

    [[nodiscard]] http_status_code status() const noexcept {
        return status_;
    }

    [[nodiscard]] std::string_view code() const noexcept {
        return code_;
    }

    [[nodiscard]] const char* what() const noexcept override {
        return message_.data();
    }

private:
    http_status_code status_;
    std::string_view code_;
    std::array<char, 128> message_{};
};

enum class http_parse_error : std::uint8_t {
    header_too_large,
    uri_too_long,
    invalid_request_line,
    unsupported_http_version,
    invalid_request_target,
    invalid_header,
    invalid_connection,
    invalid_upgrade,
    missing_host,
    invalid_host,
    invalid_content_length,
    conflicting_content_length,
    invalid_transfer_encoding,
    unsupported_transfer_encoding
};

[[nodiscard]] http_protocol_error http_parse_protocol_error(http_parse_error error) noexcept;

}  // namespace ruvia
