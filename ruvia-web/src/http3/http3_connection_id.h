#pragma once

#include <ngtcp2/ngtcp2.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <span>

#include "ruvia/http/http3_quic_connection.h"

// Server-chosen QUIC connection IDs (RFC 9000 §5.1) that route themselves
// without exposing a linkable worker prefix on the wire (RFC 9000 §9.5).
// Every worker of a POSIX HTTP/3 listener binds the UDP port with
// SO_REUSEPORT (on Linux steered in the kernel by SO_ATTACH_REUSEPORT_CBPF),
// and on Windows the acceptor receives every datagram. Each connection ID
// this server issues names the worker that owns the connection, masked and
// authenticated by the listener's shared secret and the ID's random suffix so
// that sibling IDs of one connection share no fixed bytes and a client-chosen
// ID only passes the tag check by chance (one in 2^16):
//
//   bytes 0..1   (owner worker index) ^ mask, big endian
//   bytes 2..3   tag: keyed 16-bit authentication tag over worker index and random bytes
//   bytes 4..19  random (16 CSPRNG bytes)
//
// A datagram whose destination ID names another worker is steered to it (by
// the kernel cBPF filter on Linux, or handed through the worker directory).
// Any other unknown ID (a client's first Initial) is hashed by its connection
// ID and the listener secret.

namespace ruvia::detail {

inline constexpr std::size_t http3_connection_id_length = 20;
using http3_secret = std::array<std::uint8_t, 32>;

// 32-bit mixing primes shared with the Linux SO_ATTACH_REUSEPORT_CBPF program.
inline constexpr std::uint32_t http3_cid_mix_prime_1 = 0x85ebca6bu;
inline constexpr std::uint32_t http3_cid_mix_prime_2 = 0xc2b2ae35u;

[[nodiscard]] constexpr std::uint16_t read_http3_be16(const std::uint8_t* bytes) noexcept {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) |
                                      static_cast<std::uint16_t>(bytes[1]));
}

[[nodiscard]] constexpr std::uint32_t read_http3_be32(const std::uint8_t* bytes) noexcept {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

// A connection ID as a map key: up to NGTCP2_MAX_CIDLEN bytes.
struct http3_cid_key final {
    std::array<std::uint8_t, NGTCP2_MAX_CIDLEN> bytes_{};
    std::uint8_t size_{0};

    [[nodiscard]] static http3_cid_key from(std::span<const std::uint8_t> value) noexcept {
        http3_cid_key key;
        key.size_ = static_cast<std::uint8_t>(value.size() < key.bytes_.size() ? value.size() : key.bytes_.size());
        std::memcpy(key.bytes_.data(), value.data(), key.size_);
        return key;
    }

    [[nodiscard]] static http3_cid_key from(const ngtcp2_cid& cid) noexcept {
        return from(std::span<const std::uint8_t>(cid.data, cid.datalen));
    }

    friend bool operator==(const http3_cid_key& left, const http3_cid_key& right) noexcept {
        return left.size_ == right.size_ && std::memcmp(left.bytes_.data(), right.bytes_.data(), left.size_) == 0;
    }
};

struct http3_cid_key_hash final {
    [[nodiscard]] std::size_t operator()(const http3_cid_key& key) const noexcept;
};

// Fills `cid` with a fresh ID owned by `worker_index`. False when the CSPRNG
// failed.
[[nodiscard]] bool make_http3_connection_id(
    const http3_secret& secret, std::size_t worker_index, ngtcp2_cid& cid) noexcept;

// The worker a destination connection ID names, or nullopt when it is not an
// ID of this listener (wrong length, bad tag) or names no running worker.
[[nodiscard]] std::optional<std::size_t> http3_connection_id_owner(
    const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept;
// The worker that opens the connection whose client Initial carries the
// client-chosen destination ID `cid`: a hash of it keyed by the listener
// secret. Retransmissions of that Initial carry the same ID, so they reach
// the same worker without per-connection acceptor state, and a client that
// does not know the secret cannot choose its worker.
[[nodiscard]] std::size_t http3_initial_worker(
    const http3_secret& secret, std::span<const std::uint8_t> cid, std::size_t worker_count) noexcept;

// The stateless reset token of `cid` (RFC 9000 §10.3.2), derived from the
// listener secret so every worker computes the same token.
[[nodiscard]] bool make_http3_stateless_reset_token(
    const http3_secret& secret, const ngtcp2_cid& cid, http3_quic_stateless_reset_token& token) noexcept;

}  // namespace ruvia::detail
