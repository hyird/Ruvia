#include "ruvia/web/detail/server/TrustedProxies.h"

#include <asio/ip/address.hpp>
#include <asio/ip/address_v6.hpp>

#include <string>
#include <system_error>

namespace ruvia::detail {

namespace {

// Everything is compared in IPv6 form; an IPv4 address becomes its IPv4-mapped
// equivalent so one masked compare serves both families and a deployment that
// writes 10.0.0.0/8 still matches a peer that arrives as ::ffff:10.1.2.3.
[[nodiscard]] bool toMappedBytes(std::string_view text, std::array<std::uint8_t, 16>& out, bool& wasV4) noexcept {
    std::error_code ec;
    const auto address = asio::ip::make_address(std::string(text), ec);
    if (ec) {
        return false;
    }
    if (address.is_v4()) {
        const auto mapped = asio::ip::address_v6::v4_mapped(address.to_v4()).to_bytes();
        std::copy(mapped.begin(), mapped.end(), out.begin());
        wasV4 = true;
        return true;
    }
    const auto bytes = address.to_v6().to_bytes();
    std::copy(bytes.begin(), bytes.end(), out.begin());
    wasV4 = address.to_v6().is_v4_mapped();
    return true;
}

}  // namespace

bool parseTrustedProxyBlock(std::string_view cidr, TrustedProxyBlock& out) noexcept {
    const auto slash = cidr.find('/');
    const auto addressText = slash == std::string_view::npos ? cidr : cidr.substr(0, slash);

    std::array<std::uint8_t, 16> bytes{};
    bool wasV4 = false;
    if (!toMappedBytes(addressText, bytes, wasV4)) {
        return false;
    }

    // A prefix written for an IPv4 address counts IPv4 bits; shift it past the
    // 96-bit mapping prefix so /8 means the same thing in both notations.
    const unsigned maxBits = wasV4 ? 32 : 128;
    unsigned bits = maxBits;
    if (slash != std::string_view::npos) {
        const auto prefixText = cidr.substr(slash + 1);
        if (prefixText.empty() || prefixText.size() > 3) {
            return false;
        }
        bits = 0;
        for (const char digit : prefixText) {
            if (digit < '0' || digit > '9') {
                return false;
            }
            bits = bits * 10 + static_cast<unsigned>(digit - '0');
        }
        if (bits > maxBits) {
            return false;
        }
    }

    out.network = bytes;
    out.prefixBits = static_cast<std::uint8_t>(wasV4 ? bits + 96 : bits);
    return true;
}

bool trustedProxyBlockContains(const TrustedProxyBlock& block, std::string_view peerAddress) noexcept {
    std::array<std::uint8_t, 16> peer{};
    bool wasV4 = false;
    if (!toMappedBytes(peerAddress, peer, wasV4)) {
        return false;
    }

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

}  // namespace ruvia::detail
