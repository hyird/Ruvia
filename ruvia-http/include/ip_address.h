#pragma once

#include <array>
#include <compare>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "borrowed_text.h"
#include "checked_integer.h"

namespace ruvia {

namespace detail {
struct port_number_policy final {
    static constexpr std::uint16_t default_value = 0;
    static constexpr bool implicit_conversion = true;
    static constexpr const char* invalid_value_message = "port number must be in 0..65535";

    template <std::integral integer_type>
    [[nodiscard]] static constexpr bool is_valid_value(integer_type value) noexcept {
        return std::in_range<std::uint16_t>(+value);
    }
};
}  // namespace detail

// A transport-neutral 16-bit port value shared by HTTP authorities and Web
// endpoint configurations. Zero is representable; each consumer owns its
// zero-port policy. Absence remains optional, distinct from an explicit zero.
// Numeric configuration remains convenient: integers convert implicitly, after
// the original value is checked.
class port_number final : public checked_integer<port_number, std::uint16_t, detail::port_number_policy> {
public:
    using checked_integer::checked_integer;
};

static_assert(std::is_trivially_copyable_v<port_number>);
static_assert(sizeof(port_number) == sizeof(std::uint16_t));

enum class ip_address_family : std::uint8_t {
    ipv4,
    ipv6,
};

// A numeric IPv4 or IPv6 address held as network-order bytes, independent of
// any socket library. An IPv6 address may carry a numeric zone (RFC 4007 §11)
// for link-local peers; zero means none. An IPv4-mapped IPv6 address
// (RFC 4291 §2.5.5.2) is a distinct IPv6 value here; a consumer that treats it
// as its IPv4 form states so.
class ip_address final {
public:
    // 0.0.0.0
    constexpr ip_address() noexcept = default;

    [[nodiscard]] static constexpr ip_address ipv4(const std::array<std::uint8_t, 4>& bytes) noexcept {
        ip_address result;
        for (std::size_t i = 0; i < bytes.size(); ++i) {
            result.bytes_[i] = bytes[i];
        }
        return result;
    }

    [[nodiscard]] static constexpr ip_address ipv6(const std::array<std::uint8_t, 16>& bytes, std::uint32_t scope_id = 0) noexcept {
        ip_address result;
        result.bytes_ = bytes;
        result.scope_id_ = scope_id;
        result.family_ = ip_address_family::ipv6;
        return result;
    }

    // A complete address literal: dotted-decimal IPv4 without leading zeros,
    // or RFC 4291 §2.2 IPv6 text with an optional numeric "%zone". Brackets,
    // ports, surrounding space and embedded NUL yield std::nullopt.
    [[nodiscard]] static std::optional<ip_address> try_parse(std::string_view text) noexcept;

    explicit ip_address(std::string_view text);

    [[nodiscard]] constexpr ip_address_family family() const noexcept {
        return family_;
    }

    // 4 bytes for IPv4, 16 for IPv6, in network byte order.
    [[nodiscard]] constexpr std::span<const std::uint8_t> bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return std::span<const std::uint8_t>(bytes_.data(), family_ == ip_address_family::ipv4 ? 4 : 16);
    }
    std::span<const std::uint8_t> bytes() const&& = delete;

    [[nodiscard]] constexpr std::uint32_t scope_id() const noexcept {
        return scope_id_;
    }
    [[nodiscard]] constexpr bool is_ipv4_mapped() const noexcept {
        return family_ == ip_address_family::ipv6 && scope_id_ == 0 && bytes_[0] == 0 && bytes_[1] == 0 && bytes_[2] == 0 && bytes_[3] == 0 && bytes_[4] == 0 && bytes_[5] == 0 && bytes_[6] == 0 && bytes_[7] == 0 && bytes_[8] == 0 && bytes_[9] == 0 && bytes_[10] == 0xff && bytes_[11] == 0xff;
    }

    [[nodiscard]] constexpr ip_address normalized_ipv4() const noexcept {
        if (is_ipv4_mapped()) {
            return ipv4({bytes_[12], bytes_[13], bytes_[14], bytes_[15]});
        }
        return *this;
    }

    // Network-order numeric value (a.b.c.d = 0xaabbccdd). Only native IPv4
    // has a value; call normalized_ipv4() explicitly for mapped addresses.
    [[nodiscard]] constexpr std::optional<std::uint32_t> ipv4_uint32() const noexcept {
        if (family_ != ip_address_family::ipv4) {
            return std::nullopt;
        }
        return (std::uint32_t{bytes_[0]} << 24) | (std::uint32_t{bytes_[1]} << 16) | (std::uint32_t{bytes_[2]} << 8) | bytes_[3];
    }

    // Classification treats unscoped IPv4-mapped IPv6 as IPv4. Scoped IPv6
    // retains IPv6 semantics; any scope makes public_unicast false.
    [[nodiscard]] bool is_loopback() const noexcept;
    [[nodiscard]] bool is_multicast() const noexcept;
    [[nodiscard]] bool is_link_local() const noexcept;

    // Conservative, fixed special-purpose exclusion policy, not a routing or
    // reachability test. IPv4 excludes 0/8, private/shared/loopback/link-local,
    // 192.0.0/24 (except .9/.10), documentation, 192.88.99/24, benchmarking,
    // multicast and reserved space. IPv6 admits only 2000::/3, excluding
    // 2001::/23 (protocol assignments), 2001:db8::/32, 2002::/16,
    // 3ffe::/16 (former 6bone) and 3fff::/20.
    // Thus unspecified, compatible/translation, discard, ULA, site/link-local,
    // multicast and reserved addresses are not public unicast.
    [[nodiscard]] bool is_public_unicast() const noexcept;

    // Longest text: eight four-digit groups, seven colons, then '%' and the
    // ten digits of the largest zone.
    static constexpr std::size_t max_text_size = 50;

    // RFC 5952 text: dotted decimal for IPv4; for IPv6 lowercase hex with the
    // longest zero run compressed, IPv4-mapped as "::ffff:a.b.c.d", and a zone
    // as "%<scope>". Writes the text at the start of `output` and returns that
    // prefix without allocating.
    [[nodiscard]] std::string_view format_to(std::span<char, max_text_size> output RUVIA_LIFETIMEBOUND) const noexcept;

    // The format_to() text as an owning string.
    [[nodiscard]] std::string to_string() const;

    friend constexpr bool operator==(const ip_address&, const ip_address&) noexcept = default;
    friend constexpr auto operator<=>(const ip_address&, const ip_address&) noexcept = default;

private:
    ip_address_family family_{ip_address_family::ipv4};
    std::array<std::uint8_t, 16> bytes_{};
    std::uint32_t scope_id_{0};
};

// An address and port of one transport endpoint. Port zero means the port is
// not known (for example a forwarded client address that recorded none).
class ip_endpoint final {
public:
    constexpr ip_endpoint() noexcept = default;
    constexpr ip_endpoint(ip_address address, port_number port) noexcept :
        address_(address),
        port_(port) {}

    [[nodiscard]] constexpr ip_address address() const noexcept {
        return address_;
    }

    [[nodiscard]] constexpr port_number port() const noexcept {
        return port_;
    }

    friend constexpr bool operator==(const ip_endpoint&, const ip_endpoint&) noexcept = default;

private:
    ip_address address_{};
    port_number port_{};
};

static_assert(std::is_trivially_copyable_v<ip_address>);
static_assert(std::is_trivially_copyable_v<ip_endpoint>);

// An IPv4 or IPv6 CIDR prefix block (or bare address with full-width prefix).
// Parsing requires zero host bits and rejects IPv6 zones, including "%0".
// IPv4 blocks match native and unscoped IPv4-mapped IPv6 addresses. IPv6
// blocks keep their family and prefix width; membership ignores peer scopes.
class ip_cidr final {
public:
    constexpr ip_cidr() noexcept = default;

    [[nodiscard]] static std::optional<ip_cidr> try_parse(std::string_view text) noexcept;

    explicit ip_cidr(std::string_view text);

    [[nodiscard]] ip_address network() const noexcept;
    [[nodiscard]] constexpr std::uint8_t prefix_bits() const noexcept {
        return prefix_bits_;
    }
    // Inclusive endpoints, not "usable host" addresses: IPv4 /31 and /32
    // and IPv6 /127 and /128 have no special subtraction.
    [[nodiscard]] ip_address first_address() const noexcept {
        return network();
    }
    [[nodiscard]] ip_address last_address() const noexcept;

    [[nodiscard]] bool contains(const ip_address& address) const noexcept;
    [[nodiscard]] bool contains(std::string_view address_text) const noexcept;

    friend constexpr bool operator==(const ip_cidr&, const ip_cidr&) noexcept = default;

private:
    std::array<std::uint8_t, 16> network_{};
    std::uint8_t prefix_bits_{128};
    ip_address_family family_{ip_address_family::ipv6};
};

static_assert(std::is_trivially_copyable_v<ip_cidr>);

}  // namespace ruvia
