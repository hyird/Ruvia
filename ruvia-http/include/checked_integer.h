#pragma once

#include <compare>
#include <concepts>
#include <optional>
#include <stdexcept>

namespace ruvia {

// The single checked-integer mechanism behind this library's strong integer
// types (port_number, http_status_code, websocket_close_code, ...). A boundary
// integer is validated in its original domain before it narrows to
// representation_type, so a negative or wider input cannot wrap into an
// accepted value. There is no implicit conversion back to an integer: unwrap
// value() only at a native or formatting boundary.
//
// policy_type states the accepted values and the conversion contract:
//   template <std::integral integer_type>
//   static constexpr bool is_valid_value(integer_type value) noexcept;
//   static constexpr const char* invalid_value_message;  // thrown by the constructor
//   static constexpr bool implicit_conversion;           // integer -> type without a cast
//   static constexpr representation_type default_value;  // optional: enables default construction
//
// A strong type derives from its own instantiation and inherits the
// constructors:
//   class port_number final : public checked_integer<port_number, std::uint16_t, detail::port_number_policy> {
//   public:
//       using checked_integer::checked_integer;
//   };
template <typename derived_type, std::unsigned_integral representation_type, typename policy_type>
class checked_integer {
public:
    constexpr checked_integer() noexcept
        requires requires { policy_type::default_value; }
        :
        value_(policy_type::default_value) {}

    // Local strict entry: a rejected value throws std::invalid_argument.
    template <std::integral integer_type>
    explicit(!policy_type::implicit_conversion) constexpr checked_integer(integer_type value) :
        value_(policy_type::is_valid_value(value) ? static_cast<representation_type>(value) : throw std::invalid_argument(policy_type::invalid_value_message)) {}

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return policy_type::is_valid_value(value);
    }

    // Wire-tolerant entry: a rejected value is std::nullopt.
    template <std::integral integer_type>
    [[nodiscard]] static constexpr std::optional<derived_type> try_from_value(integer_type value) noexcept {
        if (!policy_type::is_valid_value(value)) {
            return std::nullopt;
        }
        return derived_type(static_cast<representation_type>(value), validated_tag_type{});
    }

    [[nodiscard]] constexpr representation_type value() const noexcept {
        return value_;
    }

    [[nodiscard]] friend constexpr bool operator==(const derived_type& left, const derived_type& right) noexcept {
        return left.value() == right.value();
    }

    [[nodiscard]] friend constexpr std::strong_ordering operator<=>(const derived_type& left, const derived_type& right) noexcept {
        return left.value() <=> right.value();
    }

private:
    struct validated_tag_type {};

    constexpr checked_integer(representation_type value, validated_tag_type) noexcept :
        value_(value) {}

    representation_type value_;
};

}  // namespace ruvia
