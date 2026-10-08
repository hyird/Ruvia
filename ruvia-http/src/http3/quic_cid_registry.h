#pragma once

#include <cstddef>
#include <span>
#include <stdexcept>

#include "ruvia/http/quic_types.h"

namespace ruvia::detail {

// Bound once to an address-stable server entry before ngtcp2 can issue or retire
// CIDs. The view is borrowed by the connection state until its TLS driver retires.
struct quic_cid_registry_view final {
    void* context{};
    void (*publish)(void* context, std::span<const std::byte> cid){};
    void (*retire)(void* context, std::span<const std::byte> cid) noexcept {};
    void (*retire_all)(void* context) noexcept {};

    [[nodiscard]] explicit operator bool() const noexcept {
        return context != nullptr && publish != nullptr && retire != nullptr && retire_all != nullptr;
    }
};

inline void quic_publish_connection_id(quic_cid_registry_view registry,
    std::span<const std::byte> cid) {
    if (!registry || cid.size() > quic_max_connection_id_size) {
        throw std::invalid_argument("invalid QUIC server CID publication");
    }
    registry.publish(registry.context, cid);
}

inline void quic_retire_connection_id(quic_cid_registry_view registry,
    std::span<const std::byte> cid) noexcept {
    if (registry && cid.size() <= quic_max_connection_id_size) {
        registry.retire(registry.context, cid);
    }
}

inline void quic_retire_all_connection_ids(quic_cid_registry_view registry) noexcept {
    if (registry) {
        registry.retire_all(registry.context);
    }
}

}  // namespace ruvia::detail
