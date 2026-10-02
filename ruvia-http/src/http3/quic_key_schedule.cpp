#include "ruvia/http/detail/http3/quic_key_schedule.h"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>

namespace ruvia::detail {
namespace {

constexpr std::array<std::byte, 20> initial_salt{
    std::byte{0x38}, std::byte{0x76}, std::byte{0x2c}, std::byte{0xf7}, std::byte{0xf5},
    std::byte{0x59}, std::byte{0x34}, std::byte{0xb3}, std::byte{0x4d}, std::byte{0x17},
    std::byte{0x9a}, std::byte{0xe6}, std::byte{0xa4}, std::byte{0xc8}, std::byte{0x0c},
    std::byte{0xad}, std::byte{0xcc}, std::byte{0xbb}, std::byte{0x7f}, std::byte{0x0a}};
class erase_guard final {
public:
    erase_guard(quic_crypto_provider_view provider, std::span<std::byte> bytes) noexcept
        : provider_(provider),
          bytes_(bytes) {}
    erase_guard(const erase_guard&) = delete;
    erase_guard& operator=(const erase_guard&) = delete;
    ~erase_guard() noexcept {
        if (!bytes_.empty()) {
            provider_.secure_erase(provider_.context, bytes_);
        }
    }
    void release() noexcept {
        bytes_ = {};
    }

private:
    quic_crypto_provider_view provider_;
    std::span<std::byte> bytes_;
};

std::pmr::vector<std::byte> make_bytes(std::pmr::memory_resource* resource, std::size_t size) {
    std::pmr::vector<std::byte> bytes(resource);
    bytes.resize(size);
    return bytes;
}

std::pmr::memory_resource* require_resource(quic_crypto_provider_view provider,
    std::pmr::memory_resource* resource) {
    provider.validate();
    if (resource == nullptr) {
        throw std::invalid_argument("QUIC key schedule requires a memory resource");
    }
    return resource;
}

void validate_owner(quic_crypto_provider_view provider, std::pmr::memory_resource* resource) {
    (void)require_resource(provider, resource);
}

void derive_label(quic_crypto_provider_view provider, quic_cipher_suite suite,
    std::span<const std::byte> secret, std::string_view label,
    std::span<std::byte> output) {
    hkdf_expand_label(provider, suite, secret, label, {}, output);
}

}  // namespace

quic_cipher_suite_parameters quic_cipher_suite_parameters_for(quic_cipher_suite suite) {
    constexpr std::uint64_t aes_max_encryptions = std::uint64_t{1} << 23;
    constexpr std::uint64_t aes_max_decryption_failures = std::uint64_t{1} << 52;
    constexpr std::uint64_t chacha_max_encryptions = std::uint64_t{1} << 62;
    constexpr std::uint64_t chacha_max_decryption_failures = std::uint64_t{1} << 36;
    switch (suite) {
        case quic_cipher_suite::aes_128_gcm_sha256:
            return {.hash_size = 32, .key_size = 16, .iv_size = 12, .tag_size = 16, .max_encryptions = aes_max_encryptions, .max_decryption_failures = aes_max_decryption_failures};
        case quic_cipher_suite::aes_256_gcm_sha384:
            return {.hash_size = 48, .key_size = 32, .iv_size = 12, .tag_size = 16, .max_encryptions = aes_max_encryptions, .max_decryption_failures = aes_max_decryption_failures};
        case quic_cipher_suite::chacha20_poly1305_sha256:
            return {.hash_size = 32, .key_size = 32, .iv_size = 12, .tag_size = 16, .max_encryptions = chacha_max_encryptions, .max_decryption_failures = chacha_max_decryption_failures};
    }
    throw std::invalid_argument("unsupported QUIC TLS cipher suite");
}

quic_secret::quic_secret(quic_crypto_provider_view provider,
    std::pmr::memory_resource* resource, std::span<const std::byte> bytes)
    : provider_(provider),
      resource_(require_resource(provider, resource)),
      bytes_(resource_) {
    bytes_.assign(bytes.begin(), bytes.end());
}

quic_secret::quic_secret(quic_secret&& other) noexcept
    : provider_(std::exchange(other.provider_, {})),
      resource_(std::exchange(other.resource_, nullptr)),
      bytes_(std::move(other.bytes_)) {}

quic_secret::~quic_secret() noexcept {
    reset();
}

std::span<const std::byte> quic_secret::view() const noexcept {
    return bytes_;
}

void quic_secret::erase(std::span<std::byte> bytes) const noexcept {
    if (!bytes.empty()) {
        provider_.secure_erase(provider_.context, bytes);
    }
}

void quic_secret::reset() noexcept {
    if (!bytes_.empty() && provider_.secure_erase != nullptr) {
        provider_.secure_erase(provider_.context, bytes_);
    }
    bytes_.clear();
    resource_ = nullptr;
    provider_ = {};
}

quic_initial_secrets::quic_initial_secrets(quic_secret client_secret,
    quic_secret server_secret) noexcept
    : client(std::move(client_secret)),
      server(std::move(server_secret)) {}

quic_initial_secrets derive_quic_v1_initial_secrets(quic_crypto_provider_view provider,
    std::pmr::memory_resource* resource, std::span<const std::byte> initial_destination_connection_id) {
    validate_owner(provider, resource);
    if (initial_destination_connection_id.size() > quic_max_connection_id_size) {
        throw std::invalid_argument("QUIC Initial destination connection ID exceeds 20 bytes");
    }
    constexpr auto suite = quic_cipher_suite::aes_128_gcm_sha256;
    std::array<std::byte, 32> initial_secret{};
    erase_guard initial_secret_guard(provider, initial_secret);
    provider.hkdf_extract(provider.context, suite, initial_salt,
        initial_destination_connection_id, initial_secret);

    std::array<std::byte, 32> client_secret{};
    erase_guard client_secret_guard(provider, client_secret);
    derive_label(provider, suite, initial_secret, "client in", client_secret);
    quic_secret owned_client(provider, resource, client_secret);

    std::array<std::byte, 32> server_secret{};
    erase_guard server_secret_guard(provider, server_secret);
    derive_label(provider, suite, initial_secret, "server in", server_secret);
    quic_secret owned_server(provider, resource, server_secret);
    return {std::move(owned_client), std::move(owned_server)};
}

void hkdf_expand_label(quic_crypto_provider_view provider, quic_cipher_suite suite,
    std::span<const std::byte> secret, std::string_view label,
    std::span<const std::byte> context, std::span<std::byte> output) {
    provider.validate();
    const auto parameters = quic_cipher_suite_parameters_for(suite);
    if (secret.size() != parameters.hash_size) {
        throw std::invalid_argument("QUIC traffic secret has the wrong hash length");
    }
    if (output.size() > std::numeric_limits<std::uint16_t>::max() ||
        label.size() > 249 || context.size() > 255) {
        throw std::invalid_argument("TLS 1.3 HKDF label field exceeds its encoded length");
    }
    const auto full_label_size = label.size() + 6;
    std::array<std::byte, 2 + 1 + 255 + 1 + 255> info{};
    std::size_t offset = 0;
    info[offset++] = static_cast<std::byte>((output.size() >> 8) & 0xff);
    info[offset++] = static_cast<std::byte>(output.size() & 0xff);
    info[offset++] = static_cast<std::byte>(full_label_size);
    constexpr std::string_view prefix = "tls13 ";
    for (const char value : prefix) {
        info[offset++] = static_cast<std::byte>(static_cast<unsigned char>(value));
    }
    for (const char value : label) {
        info[offset++] = static_cast<std::byte>(static_cast<unsigned char>(value));
    }
    info[offset++] = static_cast<std::byte>(context.size());
    std::ranges::copy(context, info.begin() + static_cast<std::ptrdiff_t>(offset));
    offset += context.size();
    provider.hkdf_expand(provider.context, suite, secret,
        std::span<const std::byte>(info.data(), offset), output);
}

quic_packet_keys::quic_packet_keys(quic_secret traffic_secret,
    std::pmr::memory_resource* resource, std::pmr::vector<std::byte> iv,
    quic_aead_key aead, quic_header_protection_key header_protection,
    quic_cipher_suite_parameters parameters) noexcept
    : traffic_secret_(std::move(traffic_secret)),
      resource_(resource),
      iv_(std::move(iv)),
      aead_(std::move(aead)),
      header_protection_(std::move(header_protection)),
      parameters_(parameters) {}

quic_packet_keys::~quic_packet_keys() noexcept {
    traffic_secret_.erase(iv_);
}

std::span<const std::byte> quic_packet_keys::traffic_secret() const noexcept {
    return traffic_secret_.view();
}
std::span<const std::byte> quic_packet_keys::iv() const noexcept {
    return iv_;
}
std::uint64_t quic_packet_keys::max_encryptions() const noexcept {
    return parameters_.max_encryptions;
}
std::uint64_t quic_packet_keys::max_decryption_failures() const noexcept {
    return parameters_.max_decryption_failures;
}
quic_aead_key& quic_packet_keys::aead() noexcept {
    return aead_;
}
const quic_aead_key& quic_packet_keys::aead() const noexcept {
    return aead_;
}
quic_header_protection_key& quic_packet_keys::header_protection() noexcept {
    return header_protection_;
}
const quic_header_protection_key& quic_packet_keys::header_protection() const noexcept {
    return header_protection_;
}

quic_packet_keys derive_quic_packet_keys(quic_crypto_provider_view provider,
    std::pmr::memory_resource* resource, quic_cipher_suite suite,
    quic_crypto_direction direction, std::span<const std::byte> traffic_secret) {
    validate_owner(provider, resource);
    const auto parameters = quic_cipher_suite_parameters_for(suite);
    if (traffic_secret.size() != parameters.hash_size) {
        throw std::invalid_argument("QUIC traffic secret has the wrong hash length");
    }

    quic_secret owned_secret(provider, resource, traffic_secret);
    auto key_bytes = make_bytes(resource, parameters.key_size);
    erase_guard key_guard(provider, key_bytes);
    derive_label(provider, suite, traffic_secret, "quic key", key_bytes);
    auto iv = make_bytes(resource, parameters.iv_size);
    erase_guard iv_guard(provider, iv);
    derive_label(provider, suite, traffic_secret, "quic iv", iv);
    auto hp_bytes = make_bytes(resource, parameters.key_size);
    erase_guard hp_guard(provider, hp_bytes);
    derive_label(provider, suite, traffic_secret, "quic hp", hp_bytes);

    auto aead = provider.create_aead_key(provider.context, suite, direction, key_bytes);
    auto hp = provider.create_header_protection_key(provider.context, suite, hp_bytes);
    iv_guard.release();
    quic_packet_keys result(std::move(owned_secret), resource, std::move(iv),
        std::move(aead), std::move(hp), parameters);
    return result;
}

quic_updated_packet_keys::quic_updated_packet_keys(quic_secret traffic_secret,
    std::pmr::memory_resource* resource, std::pmr::vector<std::byte> iv,
    quic_aead_key aead, quic_cipher_suite_parameters parameters) noexcept
    : traffic_secret_(std::move(traffic_secret)),
      resource_(resource),
      iv_(std::move(iv)),
      aead_(std::move(aead)),
      parameters_(parameters) {}

quic_updated_packet_keys::~quic_updated_packet_keys() noexcept {
    traffic_secret_.erase(iv_);
}

std::span<const std::byte> quic_updated_packet_keys::traffic_secret() const noexcept {
    return traffic_secret_.view();
}
std::span<const std::byte> quic_updated_packet_keys::iv() const noexcept {
    return iv_;
}
std::uint64_t quic_updated_packet_keys::max_encryptions() const noexcept {
    return parameters_.max_encryptions;
}
std::uint64_t quic_updated_packet_keys::max_decryption_failures() const noexcept {
    return parameters_.max_decryption_failures;
}
quic_aead_key& quic_updated_packet_keys::aead() noexcept {
    return aead_;
}
const quic_aead_key& quic_updated_packet_keys::aead() const noexcept {
    return aead_;
}

quic_updated_packet_keys update_quic_packet_keys(quic_crypto_provider_view provider,
    std::pmr::memory_resource* resource, quic_cipher_suite suite,
    quic_crypto_direction direction, std::span<const std::byte> current_traffic_secret) {
    validate_owner(provider, resource);
    const auto parameters = quic_cipher_suite_parameters_for(suite);
    if (current_traffic_secret.size() != parameters.hash_size) {
        throw std::invalid_argument("QUIC traffic secret has the wrong hash length");
    }

    auto updated_secret_bytes = make_bytes(resource, parameters.hash_size);
    erase_guard updated_secret_guard(provider, updated_secret_bytes);
    derive_label(provider, suite, current_traffic_secret, "quic ku", updated_secret_bytes);
    quic_secret owned_secret(provider, resource, updated_secret_bytes);

    auto key_bytes = make_bytes(resource, parameters.key_size);
    erase_guard key_guard(provider, key_bytes);
    derive_label(provider, suite, updated_secret_bytes, "quic key", key_bytes);
    auto iv = make_bytes(resource, parameters.iv_size);
    erase_guard iv_guard(provider, iv);
    derive_label(provider, suite, updated_secret_bytes, "quic iv", iv);

    auto aead = provider.create_aead_key(provider.context, suite, direction, key_bytes);
    iv_guard.release();
    quic_updated_packet_keys result(std::move(owned_secret), resource,
        std::move(iv), std::move(aead), parameters);
    return result;
}

std::array<std::byte, 16> quic_v1_retry_integrity_tag(
    quic_crypto_provider_view provider, std::span<const std::byte> retry_pseudo_packet) {
    provider.validate();
    auto key = provider.create_aead_key(provider.context,
        quic_cipher_suite::aes_128_gcm_sha256, quic_crypto_direction::write,
        quic_v1_retry_integrity_key);
    std::array<std::byte, 16> tag{};
    key.seal(quic_v1_retry_integrity_nonce, retry_pseudo_packet, {}, tag);
    return tag;
}

}  // namespace ruvia::detail
