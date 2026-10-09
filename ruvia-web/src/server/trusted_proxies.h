#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <variant>
#include <vector>

#include "ruvia/core/memory/pmr_resource.h"

// Deciding who the client is when the server sits behind a reverse proxy.
//
// A forwarding header is client-controlled input: anyone can send
// X-Forwarded-For. It may be believed only when the peer that delivered it is
// one the deployment declared trustworthy, which is why this needs startup
// configuration and cannot have a useful default. With no trusted proxy
// configured the direct peer IS the client and no header is read at all -- fail
// closed, so an unconfigured server can never be told who its callers are.
//
// Addresses are stored here as raw bytes rather than asio types on purpose:
// this header reaches context_services, which is included nearly everywhere, and
// must not pull asio in behind it. Parsing and matching live in the .cpp for the
// same reason rate_limit_key.h keeps its asio dependency out of rate_limiter_type.h.

namespace ruvia::detail {

// One trusted CIDR block, parsed and validated at startup, so matching a peer on
// the request path parses each peer once before comparing the configured blocks.
struct trusted_proxy_block final {
    // IPv4 is held in its IPv4-mapped IPv6 form, so one comparison path serves
    // both families and 10.0.0.0/8 still matches ::ffff:10.1.2.3.
    std::array<std::uint8_t, 16> network_{};
    std::uint8_t prefix_bits_{0};
};

enum class trusted_proxy_parse_error : std::uint8_t { invalid_address,
    invalid_prefix };

// Parses "10.0.0.0/8", "2001:db8::/32" or a bare address (an implicit full-width
// prefix). Returns an error for anything malformed, so a typo in deployment config
// fails startup instead of silently trusting nothing.
[[nodiscard]] std::variant<trusted_proxy_block, trusted_proxy_parse_error> parse_trusted_proxy_block(std::string_view cidr) noexcept;

[[nodiscard]] bool trusted_proxy_block_contains(
    const trusted_proxy_block& block, std::string_view peer_address) noexcept;

// The startup-owned trusted set. Empty means "trust nothing", the default.
class trusted_proxy_set final {
public:
    explicit trusted_proxy_set(std::pmr::memory_resource* resource = nullptr)
        : trusted_proxy_set(resolved_pmr_resource_tag{}, pmr_resource_or_default(resource)) {}

    void add(trusted_proxy_block block) {
        blocks_.push_back(block);
    }

    void trust_x_forwarded_proto(bool enabled) noexcept {
        trust_x_forwarded_proto_ = enabled;
    }
    [[nodiscard]] bool trusts_x_forwarded_proto() const noexcept {
        return trust_x_forwarded_proto_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return blocks_.empty();
    }

    [[nodiscard]] bool trusts(std::string_view peer_address) const noexcept;

private:
    trusted_proxy_set(resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
        : blocks_(resource) {}

    std::pmr::vector<trusted_proxy_block> blocks_;
    bool trust_x_forwarded_proto_{false};
};

}  // namespace ruvia::detail
