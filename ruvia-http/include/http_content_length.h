#pragma once

#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ruvia {

// Outbound Content-Length in 0..UINT64_MAX. Checked before narrowing so
// negative inputs cannot wrap into huge finite lengths.
class http_content_length final {
public:
    constexpr http_content_length() noexcept = default;

    template <std::integral integer_type>
    constexpr http_content_length(integer_type value) :
        value_(is_valid_value(value) ? static_cast<std::uint64_t>(value) : throw std::invalid_argument("HTTP Content-Length must be in 0..18446744073709551615")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::in_range<std::uint64_t>(+value);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<http_content_length> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return http_content_length(static_cast<std::uint64_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint64_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(http_content_length, http_content_length) = default;

private:
    struct validated_tag_type {};

    constexpr http_content_length(std::uint64_t value, validated_tag_type) noexcept :
        value_(value) {}

    std::uint64_t value_{0};
};

static_assert(std::is_trivially_copyable_v<http_content_length>);
static_assert(sizeof(http_content_length) == sizeof(std::uint64_t));

// A received Content-Length field value (RFC 9110 section 8.6): a comma list
// of decimal lengths, as left by combining repeated field lines, that must all
// agree. An empty member, a non-digit, overflow or disagreeing lengths yield
// no value. Every protocol version frames messages with this same rule.
[[nodiscard]] std::optional<std::uint64_t> http_parse_content_length(std::string_view field_value) noexcept;

}  // namespace ruvia
