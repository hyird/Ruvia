#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "ruvia/http/http_header.h"

namespace ruvia {

// RFC 9218 section 4.1: urgency is 0..7, defaulting to 3. Local numeric
// inputs are checked before narrowing. Peer fields instead use the tolerant
// parser below, which ignores out-of-range values as required by section 4.
class http_priority_urgency final {
public:
    constexpr http_priority_urgency() noexcept = default;

    template <std::integral integer_type>
    constexpr http_priority_urgency(integer_type value)
        : value_(is_valid_value(value)
                     ? static_cast<std::uint8_t>(value)
                     : throw std::invalid_argument("HTTP priority urgency must be between 0 and 7")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::cmp_greater_equal(+value, 0) && std::cmp_less_equal(+value, 7);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<http_priority_urgency> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return http_priority_urgency(static_cast<std::uint8_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint8_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(http_priority_urgency, http_priority_urgency) = default;

private:
    struct validated_tag_type {};

    constexpr http_priority_urgency(std::uint8_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint8_t value_{3};
};

static_assert(std::is_trivially_copyable_v<http_priority_urgency>);
static_assert(sizeof(http_priority_urgency) == sizeof(std::uint8_t));

struct http_priority final {
    http_priority_urgency urgency_{};
    bool incremental_{false};
};
struct http_priority_fields final {
    std::optional<http_priority_urgency> urgency_{};
    std::optional<bool> incremental_{};
    [[nodiscard]] constexpr http_priority request_priority() const noexcept {
        return {urgency_.value_or(http_priority_urgency{}), incremental_.value_or(false)};
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

}  // namespace ruvia
