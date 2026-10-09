#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <variant>

namespace ruvia {

enum class http3_settings_error : std::uint8_t {
    need_more_data,
    duplicate_identifier,
    forbidden_identifier,
    value_out_of_range,
    output_too_small,
};

struct http3_settings final {
    std::uint64_t qpack_max_table_capacity_{0};
    // Absent means no advertised limit; an explicit zero forbids nonempty sections.
    std::optional<std::uint64_t> max_field_section_size_{};
    std::uint64_t qpack_blocked_streams_{0};
    // RFC 9220 SETTINGS_ENABLE_CONNECT_PROTOCOL; false is the omitted default.
    bool enable_connect_protocol_{false};
    // RFC 9297 SETTINGS_H3_DATAGRAM.
    bool h3_datagram_{false};
};

// Decodes a complete SETTINGS payload (RFC 9114 §7.2.4.1). Unknown settings are
// validated and ignored; `resource` owns temporary duplicate-detection storage.
[[nodiscard]] std::variant<http3_settings, http3_settings_error> decode_http3_settings(
    std::span<const char> payload_value,
    std::pmr::memory_resource* resource = std::pmr::get_default_resource());

// Always emits both QPACK settings; emits the field-section limit only if present
// and ENABLE_CONNECT_PROTOCOL only when enabled.
[[nodiscard]] std::variant<std::size_t, http3_settings_error> encode_http3_settings(
    std::span<char> output, const http3_settings& settings) noexcept;

}  // namespace ruvia
