#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include "ruvia/http/quic_crypto_provider.h"

namespace ruvia::detail {

// RFC 9001 Section 5.8 v1 Retry integrity parameters.
inline constexpr std::array<std::byte, 16> quic_v1_retry_integrity_key{
    std::byte{0xbe}, std::byte{0x0c}, std::byte{0x69}, std::byte{0x0b}, std::byte{0x9f},
    std::byte{0x66}, std::byte{0x57}, std::byte{0x5a}, std::byte{0x1d}, std::byte{0x76},
    std::byte{0x6b}, std::byte{0x54}, std::byte{0xe3}, std::byte{0x68}, std::byte{0xc8},
    std::byte{0x4e}};
inline constexpr std::array<std::byte, 12> quic_v1_retry_integrity_nonce{
    std::byte{0x46}, std::byte{0x15}, std::byte{0x99}, std::byte{0xd3}, std::byte{0x5d},
    std::byte{0x63}, std::byte{0x2b}, std::byte{0xf2}, std::byte{0x23}, std::byte{0x98},
    std::byte{0x25}, std::byte{0xbb}};
inline constexpr std::array<std::byte, 16> quic_v2_retry_integrity_key{
    std::byte{0x8f}, std::byte{0xb4}, std::byte{0xb0}, std::byte{0x1b}, std::byte{0x56},
    std::byte{0xac}, std::byte{0x48}, std::byte{0xe2}, std::byte{0x60}, std::byte{0xfb},
    std::byte{0xcb}, std::byte{0xce}, std::byte{0xad}, std::byte{0x7c}, std::byte{0xcc},
    std::byte{0x92}};
inline constexpr std::array<std::byte, 12> quic_v2_retry_integrity_nonce{
    std::byte{0xd8}, std::byte{0x69}, std::byte{0x69}, std::byte{0xbc}, std::byte{0x2d},
    std::byte{0x7c}, std::byte{0x6d}, std::byte{0x99}, std::byte{0x90}, std::byte{0xef},
    std::byte{0xb0}, std::byte{0x4a}};

struct quic_cipher_suite_parameters {
    std::size_t hash_size{};
    std::size_t key_size{};
    std::size_t iv_size{};
    std::size_t tag_size{};
    std::uint64_t max_encryptions{};
    std::uint64_t max_decryption_failures{};
};

quic_cipher_suite_parameters quic_cipher_suite_parameters_for(quic_cipher_suite suite);

// Owns secret bytes allocated from resource. Both must outlive this object; secret bytes are
// erased through provider before the PMR allocation is returned. This is move-only.
class quic_secret final {
public:
    quic_secret(quic_crypto_provider_view provider, std::pmr::memory_resource* resource,
        std::span<const std::byte> bytes);
    quic_secret(const quic_secret&) = delete;
    quic_secret& operator=(const quic_secret&) = delete;
    quic_secret(quic_secret&& other) noexcept;
    quic_secret& operator=(quic_secret&& other) = delete;
    ~quic_secret() noexcept;

    std::span<const std::byte> view() const noexcept;
    void erase(std::span<std::byte> bytes) const noexcept;

private:
    void reset() noexcept;

    quic_crypto_provider_view provider_{};
    std::pmr::memory_resource* resource_{};
    std::pmr::vector<std::byte> bytes_;
};

struct quic_initial_secrets final {
    quic_secret client;
    quic_secret server;

    quic_initial_secrets(quic_secret client_secret, quic_secret server_secret) noexcept;
    quic_initial_secrets(const quic_initial_secrets&) = delete;
    quic_initial_secrets& operator=(const quic_initial_secrets&) = delete;
    quic_initial_secrets(quic_initial_secrets&&) noexcept = default;
    quic_initial_secrets& operator=(quic_initial_secrets&&) = delete;
};

// RFC 9001 v1 Initial secrets derived from the Initial DCID.
quic_initial_secrets derive_quic_initial_secrets(quic_crypto_provider_view provider,
    std::pmr::memory_resource* resource, quic_version version,
    std::span<const std::byte> initial_destination_connection_id);

// Encodes the complete TLS 1.3 HKDF-Expand-Label info and expands into caller-owned output.
void hkdf_expand_label(quic_crypto_provider_view provider, quic_cipher_suite suite,
    std::span<const std::byte> secret, std::string_view label,
    std::span<const std::byte> context, std::span<std::byte> output);

class quic_packet_keys final {
public:
    quic_packet_keys(quic_secret traffic_secret, std::pmr::memory_resource* resource,
        std::pmr::vector<std::byte> iv, quic_aead_key aead,
        quic_header_protection_key header_protection,
        quic_cipher_suite_parameters parameters) noexcept;
    quic_packet_keys(const quic_packet_keys&) = delete;
    quic_packet_keys& operator=(const quic_packet_keys&) = delete;
    quic_packet_keys(quic_packet_keys&&) noexcept = default;
    quic_packet_keys& operator=(quic_packet_keys&&) = delete;
    ~quic_packet_keys() noexcept;

    std::span<const std::byte> traffic_secret() const noexcept;
    std::span<const std::byte> iv() const noexcept;
    std::uint64_t max_encryptions() const noexcept;
    std::uint64_t max_decryption_failures() const noexcept;
    quic_aead_key& aead() noexcept;
    const quic_aead_key& aead() const noexcept;
    quic_header_protection_key& header_protection() noexcept;
    const quic_header_protection_key& header_protection() const noexcept;

private:
    quic_secret traffic_secret_;
    std::pmr::memory_resource* resource_{};
    std::pmr::vector<std::byte> iv_;
    quic_aead_key aead_;
    quic_header_protection_key header_protection_;
    quic_cipher_suite_parameters parameters_{};
};

// Derives quic key/iv/hp from a TLS traffic secret. No keys or contexts are created per packet.
quic_packet_keys derive_quic_packet_keys(quic_crypto_provider_view provider,
    std::pmr::memory_resource* resource, quic_version version, quic_cipher_suite suite,
    quic_crypto_direction direction, std::span<const std::byte> traffic_secret);

class quic_updated_packet_keys final {
public:
    quic_updated_packet_keys(quic_secret traffic_secret, std::pmr::memory_resource* resource,
        std::pmr::vector<std::byte> iv, quic_aead_key aead,
        quic_cipher_suite_parameters parameters) noexcept;
    quic_updated_packet_keys(const quic_updated_packet_keys&) = delete;
    quic_updated_packet_keys& operator=(const quic_updated_packet_keys&) = delete;
    quic_updated_packet_keys(quic_updated_packet_keys&&) noexcept = default;
    quic_updated_packet_keys& operator=(quic_updated_packet_keys&&) = delete;
    ~quic_updated_packet_keys() noexcept;

    std::span<const std::byte> traffic_secret() const noexcept;
    std::span<const std::byte> iv() const noexcept;
    std::uint64_t max_encryptions() const noexcept;
    std::uint64_t max_decryption_failures() const noexcept;
    quic_aead_key& aead() noexcept;
    const quic_aead_key& aead() const noexcept;

private:
    quic_secret traffic_secret_;
    std::pmr::memory_resource* resource_{};
    std::pmr::vector<std::byte> iv_;
    quic_aead_key aead_;
    quic_cipher_suite_parameters parameters_{};
};

// RFC 9001 key update derives quic ku + quic key + quic iv. Header protection is unchanged.
quic_updated_packet_keys update_quic_packet_keys(quic_crypto_provider_view provider,
    std::pmr::memory_resource* resource, quic_version version, quic_cipher_suite suite,
    quic_crypto_direction direction, std::span<const std::byte> current_traffic_secret);

// Computes the RFC 9001 v1 Retry integrity tag over caller's pseudo-packet.
std::array<std::byte, 16> quic_retry_integrity_tag(quic_crypto_provider_view provider,
    quic_version version, std::span<const std::byte> retry_pseudo_packet);

}  // namespace ruvia::detail
