#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace ruvia {

enum class quic_role : std::uint8_t { client,
    server };
enum class quic_version : std::uint32_t { v1 = 0x00000001,
    v2 = 0x6b3343cf };
enum class quic_encryption_level : std::uint8_t { initial,
    handshake,
    application,
    early_data };
enum class quic_early_data_state : std::uint8_t { unavailable,
    available,
    accepted,
    rejected };
enum class quic_crypto_direction : std::uint8_t { read,
    write };
enum class quic_cipher_suite : std::uint16_t {
    aes_128_gcm_sha256 = 0x1301,
    aes_256_gcm_sha384 = 0x1302,
    chacha20_poly1305_sha256 = 0x1303
};

enum class quic_address_family : std::uint8_t { ipv4,
    ipv6 };
enum class quic_connection_state : std::uint8_t { connecting,
    ready,
    failed,
    closing,
    draining,
    retired };
enum class quic_operation_status : std::uint8_t {
    accepted,
    would_block,
    need_input,
    completed,
    // The stream's send direction is closed; the connection may remain active.
    stream_closed,
    closing,
    draining,
    retired
};

enum class quic_migration_status : std::uint8_t {
    started,
    would_block,
    rejected,
    validated,
    failed,
    aborted
};

enum class quic_close_kind : std::uint8_t { transport,
    application,
    tls };
enum class quic_error_code : std::uint8_t {
    invalid_configuration,
    protocol_failure,
    crypto_failure,
    resource_limit,
    invalid_state,
    invalid_stream
};

class quic_error final : public std::runtime_error {
public:
    quic_error(quic_error_code code, std::string message)
        : std::runtime_error(std::move(message)),
          code_(code) {}

    quic_error_code code() const noexcept {
        return code_;
    }

private:
    quic_error_code code_;
};
enum class quic_tls_alert : std::uint8_t {
    internal_error = 80,
    missing_extension = 109,
    no_application_protocol = 120,
    unsupported_extension = 110,
    illegal_parameter = 47,
    handshake_failure = 40,
    certificate_required = 116,
    bad_certificate = 42,
    certificate_expired = 45,
    certificate_unknown = 46,
    decrypt_error = 51,
    protocol_version = 70
};

using quic_timestamp = std::chrono::steady_clock::time_point;

inline constexpr std::size_t quic_max_connection_id_size = 20;
inline constexpr std::size_t quic_max_stream_accept_batch = 32;
inline constexpr std::size_t quic_max_alpn_size = 255;

class quic_connection_id {
public:
    quic_connection_id() noexcept = default;
    explicit quic_connection_id(std::span<const std::byte> bytes_value) {
        if (bytes_value.size() > bytes_.size()) {
            throw std::invalid_argument("QUIC connection ID exceeds 20 bytes");
        }
        std::ranges::copy(bytes_value, bytes_.begin());
        size_ = static_cast<std::uint8_t>(bytes_value.size());
    }

    std::size_t size() const noexcept {
        return size_;
    }
    std::span<const std::byte> view() const noexcept {
        return {bytes_.data(), size_};
    }

    friend bool operator==(const quic_connection_id&, const quic_connection_id&) = default;

private:
    std::array<std::byte, quic_max_connection_id_size> bytes_{};
    std::uint8_t size_{};
};

struct quic_address {
    std::array<std::byte, 16> bytes_{};
    std::uint16_t port_{};
    std::uint32_t scope_id_{};
    quic_address_family family_{quic_address_family::ipv4};
};

struct quic_transport_parameters {
    std::uint64_t idle_timeout_ms_{30000};
    std::uint64_t max_udp_payload_size_{1200};
    std::uint64_t initial_max_data_{1U << 20};
    std::uint64_t initial_max_stream_data_bidi_local_{64U << 10};
    std::uint64_t initial_max_stream_data_bidi_remote_{64U << 10};
    std::uint64_t initial_max_stream_data_uni_{64U << 10};
    std::uint64_t initial_max_streams_bidi_{100};
    std::uint64_t initial_max_streams_uni_{16};
    std::uint64_t max_datagram_frame_size_{};
    std::uint64_t active_connection_id_limit_{4};
    bool disable_active_migration_{true};
};

struct quic_limits {
    std::size_t max_datagram_size_{65527};
    std::size_t max_crypto_buffer_size_{1U << 20};
    std::size_t max_stream_buffer_size_{64U << 10};
    std::size_t max_connection_buffer_size_{1U << 20};
    std::size_t max_streams_{160};
    std::size_t max_local_streams_{32};
    std::size_t max_datagrams_{16};
    std::size_t max_lifetime_peer_streams_{128};
};

// Immutable stateless routing partition for server-generated connection IDs.
// Constructors require count > 0 and index < count; count = 1 preserves the
// unpartitioned random CID behavior.
struct quic_cid_partition {
    std::uint32_t index_{};
    std::uint32_t count_{1};
};

struct quic_connection_config {
    quic_role role_{quic_role::client};
    // The version of the first packet/Initial offer; it remains unchanged by negotiation.
    quic_version version_{quic_version::v1};
    // Local preference used when RFC 9368 compatible version negotiation selects a version.
    quic_version preferred_version_{quic_version::v1};
    quic_address local_address_{};
    quic_address peer_address_{};
    quic_connection_id destination_connection_id_{};
    std::optional<quic_connection_id> source_connection_id_{};
    std::optional<quic_connection_id> original_destination_connection_id_{};
    quic_cid_partition cid_partition_{};
    quic_transport_parameters local_transport_parameters_{};
    quic_limits limits_{};
};

struct quic_datagram_view {
    std::span<const std::byte> bytes_;
    quic_address local_;
    quic_address peer_;
};

struct quic_close_reason_view {
    quic_close_kind kind_{};
    std::uint64_t code_{};
    std::uint64_t frame_type_{};
    std::span<const char> reason_{};
};

struct quic_path_migration final {
    std::uint64_t id_{};
    quic_migration_status status_{quic_migration_status::rejected};
    quic_address local_address_{};
};

struct quic_connection_info {
    quic_connection_state state_{quic_connection_state::connecting};
    quic_version negotiated_version_{quic_version::v1};
    quic_address local_address_{};
    quic_address peer_address_{};
    bool tls_handshake_complete_{};
    bool quic_handshake_complete_{};
    bool confirmed_{};
    quic_early_data_state early_data_{quic_early_data_state::unavailable};
    std::uint64_t negotiated_idle_timeout_ms_{};
    std::uint64_t close_error_code_{};
};

struct quic_tls_info_view {
    std::span<const std::byte> negotiated_alpn_{};
    quic_cipher_suite cipher_suite_{quic_cipher_suite::aes_128_gcm_sha256};
};

struct quic_stream_metadata {
    std::uint64_t stream_id_{};
    bool readable_{};
    bool writable_{};
};

struct quic_stream_accept_batch {
    std::array<quic_stream_metadata, quic_max_stream_accept_batch> streams_{};
    std::size_t size_{};
    quic_operation_status status_{quic_operation_status::need_input};
};

enum class quic_stream_read_status : std::uint8_t { data,
    fin,
    would_block,
    reset,
    closed };

struct quic_stream_info final {
    // Sticky transport fact: at least one byte on this stream arrived in 0-RTT.
    bool received_early_data_{};
};

struct quic_stream_read_result {
    quic_stream_read_status status_{quic_stream_read_status::would_block};
    std::size_t size_{};
    std::optional<std::uint64_t> peer_reset_error_code_{};
};

struct quic_stream_write_result {
    quic_operation_status status_{quic_operation_status::would_block};
    std::size_t accepted_{};
};

enum class quic_datagram_status : std::uint8_t { received,
    would_block,
    too_large,
    unavailable };
enum class quic_datagram_write_status : std::uint8_t { queued,
    dropped,
    too_large,
    unavailable };

struct quic_datagram_result {
    quic_datagram_status status_{quic_datagram_status::would_block};
    std::size_t size_{};
    quic_address local_{};
    quic_address peer_{};
};

struct quic_packet_result {
    quic_operation_status status_{quic_operation_status::need_input};
    std::size_t size_{};
    quic_address local_{};
    quic_address peer_{};
};

struct quic_initial_offer {
    std::uint64_t offer_id_{};
    quic_address local_address_{};
    quic_address peer_address_{};
    quic_connection_id destination_connection_id_{};
    quic_connection_id source_connection_id_{};
    quic_connection_id original_destination_connection_id_{};
    quic_version version_{quic_version::v1};
};

class quic_version_negotiation_plan final {
public:
    quic_version_negotiation_plan() noexcept = default;
    quic_version_negotiation_plan(const quic_version_negotiation_plan&) = delete;
    quic_version_negotiation_plan& operator=(const quic_version_negotiation_plan&) = delete;
    quic_version_negotiation_plan(quic_version_negotiation_plan&& other) noexcept;
    quic_version_negotiation_plan& operator=(quic_version_negotiation_plan&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }

private:
    friend class quic_server;
    quic_connection_id destination_connection_id_{};
    quic_connection_id source_connection_id_{};
    quic_address local_address_{};
    quic_address peer_address_{};
    std::size_t input_size_{};
    bool valid_{};
};

enum class quic_server_route_kind : std::uint8_t { existing_connection,
    initial_offer,
    version_negotiation,
    dropped,
    rejected };

struct quic_connection_token {
    std::uint64_t value_{};
    friend bool operator==(quic_connection_token, quic_connection_token) = default;
};

struct quic_server_route {
    quic_server_route_kind kind_{quic_server_route_kind::dropped};
    quic_operation_status status_{quic_operation_status::need_input};
    quic_connection_token connection_{};
    quic_initial_offer offer_{};
    quic_version_negotiation_plan version_negotiation_{};
};

}  // namespace ruvia
