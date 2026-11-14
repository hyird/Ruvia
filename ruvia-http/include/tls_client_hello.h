#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "borrowed_text.h"

namespace ruvia {

[[nodiscard]] constexpr bool is_tls_grease_value(std::uint16_t value) noexcept {
    return (value & 0x0f0fU) == 0x0a0aU && static_cast<unsigned int>(value >> 8U) == (value & 0xffU);
}

struct tls_client_hello_fingerprints final {
    std::string ja3_;
    std::string ja3_raw_;
    std::string ja4_;
    std::string peetprint_;
    std::string peetprint_hash_;
};

// Parsed TLS ClientHello handshake fields with GREASE values filtered out.
class tls_client_hello_info final {
public:
    tls_client_hello_info() noexcept = default;
    [[nodiscard]] constexpr std::uint16_t legacy_version() const noexcept {
        return legacy_version_;
    }
    [[nodiscard]] std::uint16_t version() const noexcept;
    [[nodiscard]] std::span<const std::uint16_t> cipher_suites() const& noexcept RUVIA_LIFETIMEBOUND {
        return cipher_suites_;
    }
    std::span<const std::uint16_t> cipher_suites() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> extensions() const& noexcept RUVIA_LIFETIMEBOUND {
        return extensions_;
    }
    std::span<const std::uint16_t> extensions() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> supported_groups() const& noexcept RUVIA_LIFETIMEBOUND {
        return supported_groups_;
    }
    std::span<const std::uint16_t> supported_groups() const&& = delete;
    [[nodiscard]] std::span<const std::uint8_t> ec_point_formats() const& noexcept RUVIA_LIFETIMEBOUND {
        return ec_point_formats_;
    }
    std::span<const std::uint8_t> ec_point_formats() const&& = delete;
    [[nodiscard]] std::span<const std::string> alpn() const& noexcept RUVIA_LIFETIMEBOUND {
        return alpn_;
    }
    std::span<const std::string> alpn() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> signature_algorithms() const& noexcept RUVIA_LIFETIMEBOUND {
        return signature_algorithms_;
    }
    std::span<const std::uint16_t> signature_algorithms() const&& = delete;
    [[nodiscard]] std::span<const std::uint16_t> supported_versions() const& noexcept RUVIA_LIFETIMEBOUND {
        return supported_versions_;
    }
    std::span<const std::uint16_t> supported_versions() const&& = delete;
    [[nodiscard]] std::string_view server_name() const& noexcept RUVIA_LIFETIMEBOUND {
        return server_name_;
    }
    std::string_view server_name() const&& = delete;

    [[nodiscard]] std::string ja3_raw() const;
    [[nodiscard]] std::string ja3() const;
    [[nodiscard]] std::string ja4(bool quic = false) const;
    [[nodiscard]] std::string peetprint() const;
    [[nodiscard]] std::string peetprint_hash() const;
    [[nodiscard]] tls_client_hello_fingerprints fingerprints(bool quic = false) const;

    std::uint16_t legacy_version_{0};
    std::vector<std::uint16_t> cipher_suites_;
    std::vector<std::uint16_t> extensions_;
    std::vector<std::uint16_t> supported_groups_;
    std::vector<std::uint8_t> ec_point_formats_;
    std::vector<std::string> alpn_;
    std::vector<std::uint16_t> signature_algorithms_;
    std::vector<std::uint16_t> supported_versions_;
    std::string server_name_;
};

[[nodiscard]] std::optional<tls_client_hello_info> parse_tls_client_hello(std::span<const std::byte> message);
[[nodiscard]] std::optional<tls_client_hello_info> parse_tls_client_hello(std::string_view message);

[[nodiscard]] std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(std::span<const std::byte> message, bool quic = false);
[[nodiscard]] std::optional<tls_client_hello_fingerprints> compute_tls_client_hello_fingerprints(std::string_view message, bool quic = false);
[[nodiscard]] inline tls_client_hello_fingerprints compute_tls_client_hello_fingerprints(const tls_client_hello_info& hello, bool quic = false) {
    return hello.fingerprints(quic);
}
}  // namespace ruvia
