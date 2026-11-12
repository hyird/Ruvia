#include "http3/http3_connection_id.h"

#include <ngtcp2/ngtcp2_crypto.h>

#include <string_view>
#include <utility>

#include "ruvia/core/fnv1a.h"

#include "tls/tls_random.h"

namespace ruvia::detail {
namespace {

// Computes the 32-bit keyed digest of the 16 random bytes `cid[4..19]`. The
// high 16 bits mask the owner worker index in `cid[0..1]`; the full digest
// and the unmasked worker index feed `connection_id_tag`. Executed identically
// by the Linux SO_ATTACH_REUSEPORT_CBPF program.
[[nodiscard]] std::uint32_t connection_id_digest(const http3_secret& secret, const std::uint8_t* cid) noexcept {
    const auto k0 = read_http3_be32(secret.data());
    const auto k1 = read_http3_be32(secret.data() + 4);
    const auto k2 = read_http3_be32(secret.data() + 8);
    const auto k3 = read_http3_be32(secret.data() + 12);
    const auto w0 = read_http3_be32(cid + 4);
    const auto w1 = read_http3_be32(cid + 8);
    const auto w2 = read_http3_be32(cid + 12);
    const auto w3 = read_http3_be32(cid + 16);
    std::uint32_t acc = (w0 ^ k0) * http3_cid_mix_prime_1;
    acc = (acc ^ w1 ^ k1) * http3_cid_mix_prime_2;
    acc = (acc ^ w2 ^ k2) * http3_cid_mix_prime_1;
    acc = (acc ^ w3 ^ k3) * http3_cid_mix_prime_2;
    return acc ^ (acc >> 16);
}

[[nodiscard]] std::uint16_t connection_id_tag(
    const http3_secret& secret, std::uint32_t digest, std::uint16_t worker_index) noexcept {
    const auto k4 = read_http3_be32(secret.data() + 16);
    const std::uint32_t mixed = (digest ^ k4 ^ static_cast<std::uint32_t>(worker_index)) * http3_cid_mix_prime_1;
    return static_cast<std::uint16_t>(mixed ^ (mixed >> 16));
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
    const auto worker = static_cast<std::uint16_t>(worker_index);
    const auto digest = connection_id_digest(secret, bytes.data());
    const auto masked_worker = static_cast<std::uint16_t>(worker ^ static_cast<std::uint16_t>(digest >> 16));
    const auto tag = connection_id_tag(secret, digest, worker);
    bytes[0] = static_cast<std::uint8_t>(masked_worker >> 8);
    bytes[1] = static_cast<std::uint8_t>(masked_worker);
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
    const auto digest = connection_id_digest(secret, cid.data());
    const auto worker = static_cast<std::uint16_t>(read_http3_be16(cid.data()) ^ static_cast<std::uint16_t>(digest >> 16));
    if (static_cast<std::size_t>(worker) >= worker_count) {
        return std::nullopt;
    }
    if (read_http3_be16(cid.data() + 2) != connection_id_tag(secret, digest, worker)) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(worker);
}

std::size_t http3_initial_worker(
    const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept {
    // Match the optional Linux cBPF selector, so software dispatch keeps the
    // same owner when SO_REUSEPORT members change or no selector is available.
    if (cid.size() == http3_connection_id_length) {
        return connection_id_digest(secret, cid.data()) % worker_count;
    }
    if (cid.size() >= NGTCP2_MIN_INITIAL_DCIDLEN) {
        const auto first = read_http3_be32(cid.data()) ^ read_http3_be32(secret.data());
        const auto second = read_http3_be32(cid.data() + 4) ^ read_http3_be32(secret.data() + 4);
        const auto mixed = ((first * http3_cid_mix_prime_1) ^ second) * http3_cid_mix_prime_2;
        return (mixed >> 16) % worker_count;
    }
    auto hash = fnv1a_accumulate(fnv1a_offset_basis, std::string_view(reinterpret_cast<const char*>(secret.data()), secret.size()));
    hash = fnv1a_accumulate(hash, std::string_view(reinterpret_cast<const char*>(cid.data()), cid.size()));
    return static_cast<std::size_t>(hash % worker_count);
}

bool make_http3_stateless_reset_token(
    const http3_secret& secret, const ngtcp2_cid& cid, http3_quic_stateless_reset_token& token) noexcept {
    return ngtcp2_crypto_generate_stateless_reset_token(token.data(), secret.data(), secret.size(), &cid) == 0;
}

}  // namespace ruvia::detail
