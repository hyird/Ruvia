#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

#include "ruvia/http/borrowed_text.h"

namespace ruvia {

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

    [[nodiscard]] static constexpr ip_address ipv6(
        const std::array<std::uint8_t, 16>& bytes, std::uint32_t scope_id = 0) noexcept {
        ip_address result;
        result.bytes_ = bytes;
        result.scope_id_ = scope_id;
        result.family_ = ip_address_family::ipv6;
        return result;
    }

    // A complete address literal: dotted-decimal IPv4 without leading zeros,
    // or RFC 4291 §2.2 IPv6 text with an optional numeric "%zone". Brackets,
    // ports, surrounding space and embedded NUL yield std::nullopt.
    [[nodiscard]] static std::optional<ip_address> parse(std::string_view text) noexcept;

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

    // RFC 5952 text: dotted decimal for IPv4; for IPv6 lowercase hex with the
    // longest zero run compressed, IPv4-mapped as "::ffff:a.b.c.d", and a zone
    // as "%<scope>". nullptr selects the default memory resource.
    [[nodiscard]] std::pmr::string to_string(std::pmr::memory_resource* resource = nullptr) const;

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
    constexpr ip_endpoint(ip_address address, std::uint16_t port) noexcept
        : address_(address),
          port_(port) {}

    [[nodiscard]] constexpr ip_address address() const noexcept {
        return address_;
    }

    [[nodiscard]] constexpr std::uint16_t port() const noexcept {
        return port_;
    }

    friend constexpr bool operator==(const ip_endpoint&, const ip_endpoint&) noexcept = default;

private:
    ip_address address_{};
    std::uint16_t port_{0};
};

static_assert(std::is_trivially_copyable_v<ip_address>);
static_assert(std::is_trivially_copyable_v<ip_endpoint>);

}  // namespace ruvia
