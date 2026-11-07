#include "http3/http3_connection_id.h"

#include <ngtcp2/ngtcp2_crypto.h>

#include <string_view>
#include <utility>

#include "ruvia/core/fnv1a.h"

#include "tls/tls_random.h"

namespace ruvia::detail {
namespace {

// Only steers packets between workers: a forged tag merely sends a datagram
// to a worker that does not know its connection and drops it, so a keyed
// non-cryptographic hash suffices.
[[nodiscard]] std::uint16_t connection_id_tag(const http3_secret& secret, const std::uint8_t* cid) noexcept {
    auto hash = fnv1a_accumulate(fnv1a_offset_basis, std::string_view(reinterpret_cast<const char*>(secret.data()), secret.size()));
    hash = fnv1a_accumulate(hash, std::string_view(reinterpret_cast<const char*>(cid), 2));
    hash = fnv1a_accumulate(hash, std::string_view(reinterpret_cast<const char*>(cid + 4), http3_connection_id_length - 4));
    return static_cast<std::uint16_t>(hash ^ (hash >> 16) ^ (hash >> 32) ^ (hash >> 48));
}

}  // namespace

std::size_t http3_cid_key_hash::operator()(const http3_cid_key& key) const noexcept {
    return static_cast<std::size_t>(fnv1a_accumulate(fnv1a_offset_basis, std::string_view(reinterpret_cast<const char*>(key.bytes_.data()), key.size_)));
}

bool make_http3_connection_id(const http3_secret& secret, std::size_t worker_index, ngtcp2_cid& cid) noexcept {
    if (!std::in_range<std::uint16_t>(worker_index)) {
        return false;
    }
    std::array<std::uint8_t, http3_connection_id_length> bytes{};
    if (!fill_random_bytes(std::span<unsigned char>(bytes.data() + 4, bytes.size() - 4))) {
        return false;
    }
    bytes[0] = static_cast<std::uint8_t>(worker_index >> 8);
    bytes[1] = static_cast<std::uint8_t>(worker_index);
    const auto tag = connection_id_tag(secret, bytes.data());
    bytes[2] = static_cast<std::uint8_t>(tag >> 8);
    bytes[3] = static_cast<std::uint8_t>(tag);
    ngtcp2_cid_init(&cid, bytes.data(), bytes.size());
    return true;
}

std::optional<std::size_t> http3_connection_id_owner(
    const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept {
    if (cid.size() != http3_connection_id_length) {
        return std::nullopt;
    }
    const auto tag = static_cast<std::uint16_t>((cid[2] << 8) | cid[3]);
    if (tag != connection_id_tag(secret, cid.data())) {
        return std::nullopt;
    }
    const auto owner = static_cast<std::size_t>((cid[0] << 8) | cid[1]);
    if (owner >= worker_count) {
        return std::nullopt;
    }
    return owner;
}

#if defined(_WIN32)
std::size_t http3_initial_worker(
    const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept {
    auto hash = fnv1a_accumulate(fnv1a_offset_basis, std::string_view(reinterpret_cast<const char*>(secret.data()), secret.size()));
    hash = fnv1a_accumulate(hash, std::string_view(reinterpret_cast<const char*>(cid.data()), cid.size()));
    return static_cast<std::size_t>(hash % worker_count);
}
#endif

bool make_http3_stateless_reset_token(
    const http3_secret& secret, const ngtcp2_cid& cid, http3_quic_stateless_reset_token& token) noexcept {
    return ngtcp2_crypto_generate_stateless_reset_token(token.data(), secret.data(), secret.size(), &cid) == 0;
}

}  // namespace ruvia::detail
