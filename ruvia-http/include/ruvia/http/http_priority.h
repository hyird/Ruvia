#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <variant>

#include "ruvia/http/http_header.h"

namespace ruvia {

struct http_priority final {
    std::uint8_t urgency_{3};
    bool incremental_{false};
};
struct http_priority_fields final {
    std::optional<std::uint8_t> urgency_{};
    std::optional<bool> incremental_{};
    [[nodiscard]] constexpr http_priority request_priority() const noexcept {
        return {urgency_.value_or(3), incremental_.value_or(false)};
    }
};
enum class http_priority_error : std::uint8_t { invalid_syntax,
    invalid_value,
    invalid_frame,
    output_too_small };
struct http_priority_update final {
    std::uint64_t element_id_{0};
    bool push_{false};
    http_priority_fields fields_{};
};

// RFC 9218 / RFC 8941 Dictionary parsing. Unknown members, invalid parameter
// types and out-of-range values are ignored; malformed structured syntax fails.
// Missing response parameters remain absent for intermediary merging.
[[nodiscard]] std::variant<http_priority_fields, http_priority_error> parse_http_priority(std::string_view value) noexcept;
// Reads all Priority field lines in wire order without allocating. A later
// dictionary member replaces the earlier member, including an invalid value.
[[nodiscard]] std::variant<http_priority_fields, http_priority_error> parse_http_priority(std::span<const http_header_view> headers) noexcept;
[[nodiscard]] std::variant<std::size_t, http_priority_error> encode_http_priority(std::span<char> output, http_priority_fields fields_value) noexcept;
[[nodiscard]] std::variant<http_priority_update, http_priority_error> decode_http2_priority_update(std::span<const char> payload_value) noexcept;
[[nodiscard]] std::variant<std::size_t, http_priority_error> encode_http2_priority_update(std::span<char> output, std::uint32_t stream_id, http_priority_fields fields_value) noexcept;
[[nodiscard]] std::variant<http_priority_update, http_priority_error> decode_http3_priority_update(std::uint64_t frame_type, std::span<const char> payload_value) noexcept;
[[nodiscard]] std::variant<std::size_t, http_priority_error> encode_http3_priority_update(std::span<char> output, http_priority_update update) noexcept;

}  // namespace ruvia
