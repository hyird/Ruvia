#pragma once

#include <compare>
#include <concepts>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ruvia {

// A transport-neutral 16-bit port value shared by HTTP authorities and Web
// endpoint configurations. Zero is representable; each consumer owns its
// zero-port policy. Absence remains optional, distinct from an explicit zero.
class port_number final {
public:
    constexpr port_number() noexcept = default;

    // Numeric configuration remains convenient, but the original integer is
    // checked before narrowing. There is no implicit conversion back to an
    // integer: unwrap value() only at a native or formatting boundary.
    template <std::integral integer_type>
    constexpr port_number(integer_type value)
        : value_(is_valid_value(value)
                     ? static_cast<std::uint16_t>(value)
                     : throw std::invalid_argument("port number must be in 0..65535")) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::in_range<std::uint16_t>(+value);
    }

    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<port_number> try_from_value(integer_type value) noexcept {
        if (!is_valid_value(value)) {
            return std::nullopt;
        }
        return port_number(static_cast<std::uint16_t>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr std::uint16_t value() const noexcept {
        return value_;
    }

    friend constexpr auto operator<=>(port_number, port_number) = default;

private:
    struct validated_tag_type {};

    constexpr port_number(std::uint16_t value, validated_tag_type) noexcept
        : value_(value) {}

    std::uint16_t value_{0};
};

static_assert(std::is_trivially_copyable_v<port_number>);
static_assert(sizeof(port_number) == sizeof(std::uint16_t));

}  // namespace ruvia
