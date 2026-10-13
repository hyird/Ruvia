#include "server/trusted_proxies.h"

#include <algorithm>
#include <array>
#include <system_error>
#include <variant>

#include <asio/ip/address.hpp>
#include <asio/ip/address_v6.hpp>

#include "ruvia/core/ip_address.h"

namespace ruvia::detail {

namespace {

struct mapped_address final {
    std::array<std::uint8_t, 16> bytes_{};
    bool was_v4_{false};
};

// Everything is compared in IPv6 form; an IPv4 address becomes its IPv4-mapped
// equivalent so one masked compare serves both families and a deployment that
// writes 10.0.0.0/8 still matches a peer that arrives as ::ffff:10.1.2.3.
[[nodiscard]] std::variant<mapped_address, std::error_code> to_mapped_bytes(
    std::string_view text) noexcept {
    const auto parsed_value = ruvia::parse_ip_address(text);
    if ((parsed_value.index() != 0)) {
        return std::get<1>(parsed_value);
    }
    const auto& address = std::get<0>(parsed_value);
    if (address.is_v4()) {
        const auto mapped = asio::ip::make_address_v6(asio::ip::v4_mapped, address.to_v4()).to_bytes();
        return mapped_address{mapped, true};
    }
    const auto bytes_value = address.to_v6().to_bytes();
    return mapped_address{bytes_value, false};
}

[[nodiscard]] bool contains_mapped_address(
    const trusted_proxy_block& block, const std::array<std::uint8_t, 16>& peer) noexcept {
    const auto bits = static_cast<std::size_t>(block.prefix_bits_);
    const auto whole_bytes = bits / 8;
    for (std::size_t i = 0; i < whole_bytes; ++i) {
        if (peer[i] != block.network_[i]) {
            return false;
        }
    }
    const auto remainder = bits % 8;
    if (remainder == 0) {
        return true;
    }
    const auto mask = static_cast<std::uint8_t>(0xFF << (8 - remainder));
    return (peer[whole_bytes] & mask) == (block.network_[whole_bytes] & mask);
}

}  // namespace

std::variant<trusted_proxy_block, trusted_proxy_parse_error> parse_trusted_proxy_block(std::string_view cidr) noexcept {
    const auto slash = cidr.find('/');
    const auto address_text = slash == std::string_view::npos ? cidr : cidr.substr(0, slash);

    const auto address = to_mapped_bytes(address_text);
    if ((address.index() != 0)) {
        return trusted_proxy_parse_error::invalid_address;
    }

    // Only an IPv4 literal counts IPv4 bits. IPv6 literals, including mapped
    // addresses, count from the start of the full 128-bit address.
    const unsigned max_bits = std::get<0>(address).was_v4_ ? 32 : 128;
    unsigned bits = max_bits;
    if (slash != std::string_view::npos) {
        const auto prefix_text = cidr.substr(slash + 1);
        if (prefix_text.empty() || prefix_text.size() > 3) {
            return trusted_proxy_parse_error::invalid_prefix;
        }
        bits = 0;
        for (const char digit : prefix_text) {
            if (digit < '0' || digit > '9') {
                return trusted_proxy_parse_error::invalid_prefix;
            }
            bits = bits * 10 + static_cast<unsigned>(digit - '0');
        }
        if (bits > max_bits) {
            return trusted_proxy_parse_error::invalid_prefix;
        }
    }

    return trusted_proxy_block{std::get<0>(address).bytes_, static_cast<std::uint8_t>(std::get<0>(address).was_v4_ ? bits + 96 : bits)};
}

trusted_proxy_set::hop_trust trusted_proxy_set::classify(std::string_view address) const noexcept {
    const auto peer = to_mapped_bytes(address);
    if ((peer.index() != 0)) {
        return hop_trust::unidentified;
    }
    const auto trusted = std::ranges::any_of(blocks_, [&](const trusted_proxy_block& block) noexcept {
        return contains_mapped_address(block, std::get<0>(peer).bytes_);
    });
    return trusted ? hop_trust::trusted : hop_trust::untrusted;
}

}  // namespace ruvia::detail
