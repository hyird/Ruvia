#pragma once

#include <ngtcp2/ngtcp2.h>

#include <cstddef>
#include <cstdint>
#include <span>

#include "ruvia/http/quic_types.h"

namespace ruvia::detail {

class quic_connection_state;

// Fills QUIC crypto/TLS lifecycle callbacks only; the packet/stream owner fills its own
// callbacks on the same table. All callback adapters contain exceptions at the C ABI edge.
void fill_quic_crypto_callbacks(ngtcp2_callbacks& callbacks) noexcept;

// The rand callback has no user_data. Install the address-stable connection state here and
// pass this context to ngtcp2 settings; keep it alive until ngtcp2_conn_del completes.
void initialize_quic_random_context(ngtcp2_rand_ctx& context,
    quic_connection_state& state) noexcept;

// Installs version-specific Initial keys derived from the supplied client's Initial DCID.
// When version_negotiation is true, uses ngtcp2's compatible-version Initial-key API.
void install_quic_initial_keys(quic_connection_state& state,
    std::span<const std::byte> client_initial_dcid, bool version_negotiation = false);

// Encodes the initialized ngtcp2 connection's complete local transport parameters into
// connection-owned stable storage. The TLS capability may borrow this through retirement.
void encode_quic_local_transport_parameters(quic_connection_state& state);

// Installs one TLS-produced early, Handshake, or 1-RTT traffic secret. Key/IV/HP are derived
// by the HTTP-owned schedule; the temporary secret bytes are copied into ngtcp2 before return.
void install_quic_traffic_secret(quic_connection_state& state,
    quic_encryption_level level, quic_crypto_direction direction,
    quic_cipher_suite suite, std::span<const std::byte> secret);

// Must be called after each ngtcp2 call that can invoke a crypto callback, and before its
// output is exposed. Rethrows the first backend exception latched at the C boundary.
void rethrow_quic_callback_failure(const quic_connection_state& state);

}  // namespace ruvia::detail
