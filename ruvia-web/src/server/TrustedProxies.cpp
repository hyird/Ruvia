#include "ruvia/web/detail/server/TrustedProxies.h"

#include <algorithm>
#include <array>
#include <expected>
#include <string>
#include <system_error>

#include <asio/ip/address.hpp>
#include <asio/ip/address_v6.hpp>

#include "ruvia/core/IpAddress.h"

namespace ruvia::detail {

namespace {

struct MappedAddress final {
    std::array<std::uint8_t, 16> bytes{};
    bool wasV4{false};
};

// Everything is compared in IPv6 form; an IPv4 address becomes its IPv4-mapped
// equivalent so one masked compare serves both families and a deployment that
// writes 10.0.0.0/8 still matches a peer that arrives as ::ffff:10.1.2.3.
[[nodiscard]] std::expected<MappedAddress, std::error_code> toMappedBytes(
    std::string_view text) noexcept {
    const auto parsed = ruvia::parseIpAddress(text);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    const auto& address = *parsed;
    if (address.is_v4()) {
        const auto mapped = asio::ip::address_v6::v4_mapped(address.to_v4()).to_bytes();
        return MappedAddress{mapped, true};
    }
    const auto bytes = address.to_v6().to_bytes();
    return MappedAddress{bytes, address.to_v6().is_v4_mapped()};
}

[[nodiscard]] bool containsMappedAddress(
    const TrustedProxyBlock& block, const std::array<std::uint8_t, 16>& peer) noexcept {
    const auto bits = static_cast<std::size_t>(block.prefixBits);
    const auto wholeBytes = bits / 8;
    for (std::size_t i = 0; i < wholeBytes; ++i) {
        if (peer[i] != block.network[i]) {
            return false;
        }
    }
    const auto remainder = bits % 8;
    if (remainder == 0) {
        return true;
    }
    const auto mask = static_cast<std::uint8_t>(0xFF << (8 - remainder));
    return (peer[wholeBytes] & mask) == (block.network[wholeBytes] & mask);
}

}  // namespace

std::expected<TrustedProxyBlock, TrustedProxyParseError> parseTrustedProxyBlock(std::string_view cidr) noexcept {
    const auto slash = cidr.find('/');
    const auto addressText = slash == std::string_view::npos ? cidr : cidr.substr(0, slash);

    const auto address = toMappedBytes(addressText);
    if (!address) {
        return std::unexpected(TrustedProxyParseError::kInvalidAddress);
    }

    // A prefix written for an IPv4 address counts IPv4 bits; shift it past the
    // 96-bit mapping prefix so /8 means the same thing in both notations.
    const unsigned maxBits = address->wasV4 ? 32 : 128;
    unsigned bits = maxBits;
    if (slash != std::string_view::npos) {
        const auto prefixText = cidr.substr(slash + 1);
        if (prefixText.empty() || prefixText.size() > 3) {
            return std::unexpected(TrustedProxyParseError::kInvalidPrefix);
        }
        bits = 0;
        for (const char digit : prefixText) {
            if (digit < '0' || digit > '9') {
                return std::unexpected(TrustedProxyParseError::kInvalidPrefix);
            }
            bits = bits * 10 + static_cast<unsigned>(digit - '0');
        }
        if (bits > maxBits) {
            return std::unexpected(TrustedProxyParseError::kInvalidPrefix);
        }
    }

    return TrustedProxyBlock{address->bytes, static_cast<std::uint8_t>(address->wasV4 ? bits + 96 : bits)};
}

bool trustedProxyBlockContains(
    const TrustedProxyBlock& block, std::string_view peerAddress) noexcept {
    const auto peer = toMappedBytes(peerAddress);
    return peer && containsMappedAddress(block, peer->bytes);
}

bool TrustedProxySet::trusts(std::string_view peerAddress) const noexcept {
    if (blocks_.empty() || peerAddress.empty()) {
        return false;
    }
    const auto peer = toMappedBytes(peerAddress);
    if (!peer) {
        return false;
    }
    return std::ranges::any_of(blocks_, [&](const TrustedProxyBlock& block) noexcept {
        return containsMappedAddress(block, peer->bytes);
    });
}

}  // namespace ruvia::detail
