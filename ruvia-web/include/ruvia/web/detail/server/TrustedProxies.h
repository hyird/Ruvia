#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string_view>
#include <vector>

#include "ruvia/core/memory/PmrResource.h"

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
// this header reaches ContextServices, which is included nearly everywhere, and
// must not pull asio in behind it. Parsing and matching live in the .cpp for the
// same reason RateLimitKey.h keeps its asio dependency out of RateLimiter.h.

namespace ruvia::detail {

// One trusted CIDR block, parsed and validated at startup, so matching a peer on
// the request path is a masked compare with no parsing and no allocation.
struct TrustedProxyBlock final {
    // IPv4 is held in its IPv4-mapped IPv6 form, so one comparison path serves
    // both families and 10.0.0.0/8 still matches ::ffff:10.1.2.3.
    std::array<std::uint8_t, 16> network{};
    std::uint8_t prefixBits{0};
};

// Parses "10.0.0.0/8", "2001:db8::/32" or a bare address (an implicit full-width
// prefix). Returns false for anything malformed, so a typo in deployment config
// fails startup instead of silently trusting nothing.
[[nodiscard]] bool parseTrustedProxyBlock(std::string_view cidr, TrustedProxyBlock& out) noexcept;

[[nodiscard]] bool trustedProxyBlockContains(
    const TrustedProxyBlock& block, std::string_view peerAddress) noexcept;

// The startup-owned trusted set. Empty means "trust nothing", the default.
class TrustedProxySet final {
public:
    explicit TrustedProxySet(std::pmr::memory_resource* resource = nullptr)
        : TrustedProxySet(ResolvedPmrResourceTag{}, pmrResourceOrDefault(resource)) {}

    void add(TrustedProxyBlock block) {
        blocks_.push_back(block);
    }

    [[nodiscard]] bool empty() const noexcept {
        return blocks_.empty();
    }

    [[nodiscard]] bool trusts(std::string_view peerAddress) const noexcept {
        if (blocks_.empty() || peerAddress.empty()) {
            return false;
        }
        for (const auto& block : blocks_) {
            if (trustedProxyBlockContains(block, peerAddress)) {
                return true;
            }
        }
        return false;
    }

private:
    TrustedProxySet(ResolvedPmrResourceTag, std::pmr::memory_resource* resource)
        : blocks_(resource) {}

    std::pmr::vector<TrustedProxyBlock> blocks_;
};

}  // namespace ruvia::detail
