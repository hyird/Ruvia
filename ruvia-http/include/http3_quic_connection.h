#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "borrowed_text.h"
#include "byte_view.h"
#include "http3_connection.h"
#include "ip_address.h"

extern "C" {
struct ngtcp2_conn;
struct ngtcp2_callbacks;
}

namespace ruvia {

inline constexpr std::uint32_t quic_version_v1 = 0x00000001U;
inline constexpr std::uint32_t quic_version_v2 = 0x6b3343cfU;
inline constexpr std::size_t quic_max_cid_bytes = 20;
inline constexpr std::size_t quic_min_initial_dcid_bytes = 8;
inline constexpr std::size_t quic_stateless_reset_token_bytes = 16;
inline constexpr std::size_t quic_min_stateless_reset_random_bytes = 5;

using quic_timestamp = std::uint64_t;
using http3_quic_stateless_reset_token = std::array<std::byte, quic_stateless_reset_token_bytes>;

// Value-type QUIC Connection ID (0..20 bytes, RFC 9000 §17.2).
class quic_cid final {
public:
    static constexpr std::size_t max_bytes = quic_max_cid_bytes;

    constexpr quic_cid() noexcept = default;

    explicit constexpr quic_cid(byte_view bytes) {
        if (bytes.size() > max_bytes) {
            throw std::invalid_argument("QUIC Connection ID exceeds 20 bytes");
        }
        std::ranges::copy(bytes, data_.begin());
        size_ = static_cast<std::uint8_t>(bytes.size());
    }

    [[nodiscard]] static constexpr std::optional<quic_cid> try_from_bytes(byte_view bytes) noexcept {
        if (bytes.size() > max_bytes) {
            return std::nullopt;
        }
        quic_cid cid;
        std::ranges::copy(bytes, cid.data_.begin());
        cid.size_ = static_cast<std::uint8_t>(bytes.size());
        return cid;
    }

    [[nodiscard]] constexpr byte_view bytes() const& noexcept RUVIA_LIFETIMEBOUND {
        return byte_view(data_.data(), size_);
    }
    byte_view bytes() const&& = delete;

    [[nodiscard]] constexpr const std::byte* data() const noexcept {
        return data_.data();
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] friend constexpr bool operator==(const quic_cid& left, const quic_cid& right) noexcept {
        return left.size_ == right.size_ && std::ranges::equal(byte_view(left.data_.data(), left.size_), byte_view(right.data_.data(), right.size_));
    }

private:
    std::array<std::byte, max_bytes> data_{};
    std::uint8_t size_{0};
};

// Socket addresses of one QUIC path (RFC 9000 §9) as pure value endpoints.
struct http3_quic_path final {
    ip_endpoint local_{};
    ip_endpoint remote_{};
    std::uintptr_t user_data_{0};

    [[nodiscard]] friend constexpr bool operator==(const http3_quic_path&, const http3_quic_path&) noexcept = default;
};

struct http3_quic_datagram final {
    http3_quic_path path_{};
    std::uint32_t ecn_{0};
    std::vector<std::byte> bytes_{};
};

enum class http3_quic_status : std::uint8_t {
    accepted,
    would_block,
    // The connection entered its closing or draining period (RFC 9000 §10.2):
    // a CONNECTION_CLOSE was sent or received, or the idle timeout expired.
    // No further packet is processed; the owner keeps the connection only to
    // let late peer packets drain until close_period() elapses.
    closed,
    invalid_state,
    transport_error,
    application_error,
};

struct http3_quic_result final {
    http3_quic_status status_{http3_quic_status::accepted};
    std::optional<std::uint64_t> transport_error_code_{};
    std::optional<http3_error_code> application_error_code_{};
};

// Installs QUIC-TLS crypto callbacks on the private `ngtcp2_callbacks` table
// (for example via `<ngtcp2/ngtcp2_crypto.h>` in the TLS/I/O layer). The
// connection overwrites every stream, ACK, flow-control and lifecycle slot
// afterwards, so `ruvia-http` exposes no `<ngtcp2/ngtcp2.h>` header dependency.
using http3_quic_configure_crypto_callbacks_type = void (*)(ngtcp2_callbacks& callbacks) noexcept;

// Connection lifecycle notifications for the owner; each, when set, receives
// http3_quic_connection_options::callback_context_. Returning false fails the
// connection with a callback failure.
struct http3_quic_owner_callbacks final {
    bool (*get_new_connection_id_)(quic_cid& cid, http3_quic_stateless_reset_token& token, std::size_t cid_size, void* context) noexcept {nullptr};
    bool (*remove_connection_id_)(const quic_cid& cid, void* context) noexcept {nullptr};
    bool (*handshake_completed_)(void* context) noexcept {nullptr};
    bool (*handshake_confirmed_)(void* context) noexcept {nullptr};
    bool (*recv_stateless_reset_)(const http3_quic_stateless_reset_token& token, void* context) noexcept {nullptr};
    bool (*tls_early_data_rejected_)(void* context) noexcept {nullptr};
};

struct http3_quic_connection_options final {
    http3_quic_configure_crypto_callbacks_type configure_crypto_callbacks_{nullptr};
    http3_quic_owner_callbacks owner_callbacks_{};
    quic_cid source_connection_id_{};
    quic_cid destination_connection_id_{};
    // Server only and required (RFC 9000 §7.3): the Destination Connection ID
    // of the client's first Initial packet (extracted from the Retry token
    // when Retry was used).
    std::optional<quic_cid> original_destination_connection_id_{};
    // Server only: the Source Connection ID sent in the Retry packet that the
    // client echoed as the Destination Connection ID of its second Initial
    // (RFC 9000 §7.3, §18.2). Requires retry_token_ to be non-empty.
    std::optional<quic_cid> retry_source_connection_id_{};
    // Server only: the validated Retry token from the client's second Initial
    // (RFC 9000 §8.1.2). Lifts the anti-amplification limit in ngtcp2.
    borrowed_bytes retry_token_{};
    // Server only: the stateless reset token of source_connection_id_
    // (RFC 9000 §10.3), sent as a transport parameter.
    std::optional<http3_quic_stateless_reset_token> stateless_reset_token_{};
    // Server only: forbids client connection migration (RFC 9000 §9).
    bool disable_active_migration_{false};
    http3_quic_path initial_path_{};
    std::uint32_t version_{quic_version_v1};
    quic_timestamp initial_timestamp_{0};
    std::chrono::nanoseconds handshake_timeout_{std::chrono::seconds{30}};
    std::chrono::nanoseconds idle_timeout_{std::chrono::seconds{30}};
    std::uint64_t initial_max_data_{16 * 1024 * 1024};
    std::uint64_t initial_max_stream_data_{1024 * 1024};
    std::uint64_t initial_max_streams_bidi_{100};
    std::uint64_t initial_max_streams_uni_{16};
    std::uint64_t active_connection_id_limit_{4};
    std::size_t max_udp_payload_size_{1350};
    std::size_t max_pending_datagram_bytes_{64 * 1024};
    void* callback_context_{nullptr};
    http3_connection_options http3_{};
};

// One QUIC v1 + HTTP/3 connection. ngtcp2 owns packet, loss recovery and flow
// control; nghttp3 owns QPACK and HTTP stream framing. The caller owns UDP and
// TLS. Input datagrams must remain alive until next_event() drains their events.
class http3_quic_connection final {
public:
    [[nodiscard]] static http3_quic_connection client(const http3_quic_connection_options& options);
    [[nodiscard]] static http3_quic_connection server(const http3_quic_connection_options& options);

    ~http3_quic_connection();
    http3_quic_connection(const http3_quic_connection&) = delete;
    http3_quic_connection& operator=(const http3_quic_connection&) = delete;
    http3_quic_connection(http3_quic_connection&&) noexcept;
    http3_quic_connection& operator=(http3_quic_connection&&) noexcept;

    [[nodiscard]] http_connection_role role() const noexcept;
    // For the QUIC-TLS adapter's connection reference (ngtcp2_crypto_conn_ref::get_conn).
    [[nodiscard]] ngtcp2_conn* native() & noexcept RUVIA_LIFETIMEBOUND;
    ngtcp2_conn* native() && = delete;
    [[nodiscard]] const ngtcp2_conn* native() const& noexcept RUVIA_LIFETIMEBOUND;
    const ngtcp2_conn* native() const&& = delete;
    [[nodiscard]] void* callback_context() const noexcept;
    void set_tls_session(void* tls_session) noexcept;
    [[nodiscard]] bool handshake_completed() const noexcept;
    [[nodiscard]] bool after_retry() const noexcept;
    [[nodiscard]] bool stateless_reset_received() const noexcept;

    [[nodiscard]] http3_quic_status start_http3();
    [[nodiscard]] bool http3_started() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> open_request_stream();

    [[nodiscard]] http3_quic_status start_early_http3(byte_view transport_parameters, http3_peer_settings settings);
    [[nodiscard]] http3_quic_status reject_early_data();
    [[nodiscard]] bool early_data_rejected() const noexcept;
    [[nodiscard]] std::optional<std::size_t> encode_early_transport_parameters(mutable_byte_view output) const noexcept;

    [[nodiscard]] http3_quic_path current_path() const noexcept;
    [[nodiscard]] http3_quic_result migrate(const http3_quic_path& path, quic_timestamp timestamp);

    [[nodiscard]] http3_quic_status submit_datagram(std::int64_t stream_id, byte_view payload);
    [[nodiscard]] std::size_t max_datagram_payload_size(std::int64_t stream_id) const noexcept;
    [[nodiscard]] http3_quic_result receive_datagram(const http3_quic_path& path, std::uint32_t ecn, byte_view bytes, quic_timestamp timestamp);
    [[nodiscard]] http3_quic_result next_datagram(std::size_t capacity, quic_timestamp timestamp, http3_quic_datagram& output);
    [[nodiscard]] http3_quic_result expire_timers(quic_timestamp timestamp);
    [[nodiscard]] quic_timestamp expiry() const noexcept;

    void reset_stream(std::int64_t stream_id, http3_error_code error) noexcept;
    void reset_send_stream(std::int64_t stream_id, http3_error_code error) noexcept;
    void stop_receiving(std::int64_t stream_id, http3_error_code error) noexcept;
    void close(http3_error_code error) noexcept;
    [[nodiscard]] bool closed() const noexcept;
    [[nodiscard]] std::chrono::nanoseconds close_period() const noexcept;

    [[nodiscard]] std::optional<http3_event> next_event() &;
    std::optional<http3_event> next_event() && = delete;
    [[nodiscard]] http3_connection& http3() & noexcept RUVIA_LIFETIMEBOUND;
    http3_connection& http3() && = delete;
    [[nodiscard]] const http3_connection& http3() const& noexcept RUVIA_LIFETIMEBOUND;
    const http3_connection& http3() const&& = delete;
    [[nodiscard]] std::optional<http3_error_code> application_error() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> transport_error() const noexcept;

private:
    class impl;
    explicit http3_quic_connection(http_connection_role role, const http3_quic_connection_options& options);
    std::unique_ptr<impl> impl_;
};

enum class quic_packet_version_status : std::uint8_t {
    ok,
    version_negotiation,
    invalid,
};

struct quic_packet_version_cid final {
    quic_packet_version_status status_{quic_packet_version_status::invalid};
    std::uint32_t version_{0};
    byte_view dcid_{};
    byte_view scid_{};
};

struct quic_initial_packet_header final {
    std::uint32_t version_{0};
    quic_cid dcid_{};
    quic_cid scid_{};
    byte_view token_{};
};

// Decodes the version and connection IDs of an incoming UDP datagram for
// demultiplexing and Version Negotiation (RFC 9000 §17.2, §17.3). `dcid_` and
// `scid_` borrow `packet`.
[[nodiscard]] quic_packet_version_cid decode_quic_packet_version_cid(byte_view packet RUVIA_LIFETIMEBOUND, std::size_t short_dcid_size) noexcept;

// Validates and decodes a client Initial packet suitable for accepting a new
// server connection (RFC 9000 §7.2, §14.1). `token_` borrows `packet`.
[[nodiscard]] std::optional<quic_initial_packet_header> decode_quic_initial_packet(byte_view packet RUVIA_LIFETIMEBOUND) noexcept;

// Writes a QUIC Version Negotiation packet (RFC 9000 §17.2.1). Returns the
// number of bytes written, or 0 if `output` cannot hold the packet.
[[nodiscard]] std::size_t write_quic_version_negotiation_packet(mutable_byte_view output, std::uint8_t unused_random, byte_view dcid, byte_view scid, std::span<const std::uint32_t> supported_versions) noexcept;

// Writes a QUIC Stateless Reset packet (RFC 9000 §10.3) into `output`, using
// at most `max_packet_bytes` (pass `trigger_datagram_bytes - 1` so the packet
// is strictly smaller than the datagram that triggered it, RFC 9000 §10.3.3).
[[nodiscard]] std::size_t write_http3_quic_stateless_reset(const http3_quic_stateless_reset_token& token, byte_view random_bytes, mutable_byte_view output, std::size_t max_packet_bytes) noexcept;

}  // namespace ruvia
