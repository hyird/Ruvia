#pragma once

#include <ngtcp2/ngtcp2.h>

#include "ruvia/http/quic_types.h"

namespace ruvia::detail {

// Converts an address into caller-owned ngtcp2 storage. The returned addr points
// into storage and remains valid only while storage remains alive and unchanged.
[[nodiscard]] ngtcp2_addr encode_quic_address(ngtcp2_sockaddr_union& storage,
    const quic_address& address);

// Decodes a native ngtcp2 address, rejecting null, unsupported, and malformed inputs.
[[nodiscard]] quic_address decode_quic_address(const ngtcp2_addr& address);

// Initializes path with copied endpoint addresses; its embedded buffers own the result.
void fill_quic_path(ngtcp2_path_storage& path, const quic_address& local,
    const quic_address& peer);

}  // namespace ruvia::detail
