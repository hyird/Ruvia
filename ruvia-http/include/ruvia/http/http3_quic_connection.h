#pragma once

#include <ngtcp2/ngtcp2.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include "ruvia/http/http3_connection.h"

namespace ruvia {

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
    std::uint64_t error_code_{0};
};

using http3_quic_stateless_reset_token =
    std::array<std::uint8_t, NGTCP2_STATELESS_RESET_TOKENLEN>;

struct http3_quic_connection_options final {
    // Crypto callbacks come from the selected ngtcp2 crypto adapter. The
    // wrapper owns stream-data, acknowledgement and stream-close callbacks.
    // get_new_connection_id2, remove_connection_id, handshake_completed,
    // handshake_confirmed and tls_early_data_rejected, when set, receive callback_user_data_.
    ngtcp2_callbacks callbacks_{};
    ngtcp2_cid source_connection_id_{};
    ngtcp2_cid destination_connection_id_{};
    // Server only and required (RFC 9000 §7.3): the Destination Connection ID
    // of the client's first Initial packet. Retry is not supported, so this
    // is always the connection ID the client chose.
    std::optional<ngtcp2_cid> original_destination_connection_id_{};
    // Server only: the stateless reset token of source_connection_id_
    // (RFC 9000 §10.3), sent as a transport parameter.
    std::optional<http3_quic_stateless_reset_token> stateless_reset_token_{};
    // Server only: forbids client connection migration (RFC 9000 §9).
    // Leave false when the UDP owner routes connection IDs across paths.
    bool disable_active_migration_{false};
    ngtcp2_path initial_path_{};
    std::uint32_t version_{NGTCP2_PROTO_VER_V1};
    ngtcp2_tstamp initial_timestamp_{0};
    ngtcp2_duration handshake_timeout_{30 * NGTCP2_SECONDS};
    ngtcp2_duration idle_timeout_{30 * NGTCP2_SECONDS};
    std::uint64_t initial_max_data_{16 * 1024 * 1024};
    std::uint64_t initial_max_stream_data_{1024 * 1024};
    std::uint64_t initial_max_streams_bidi_{100};
    std::uint64_t initial_max_streams_uni_{16};
    std::uint64_t active_connection_id_limit_{2};
    std::size_t max_udp_payload_size_{1350};
    // Bounded unreliable output retained until ngtcp2 accepts each frame.
    std::size_t max_pending_datagram_bytes_{64 * 1024};
    void* callback_user_data_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    http3_connection_options http3_{};
};

struct http3_quic_datagram final {
    explicit http3_quic_datagram(
        std::pmr::memory_resource* resource = nullptr);
    http3_quic_datagram(const http3_quic_datagram&) = delete;
    http3_quic_datagram& operator=(const http3_quic_datagram&) = delete;
    http3_quic_datagram(http3_quic_datagram&&) = delete;
    http3_quic_datagram& operator=(http3_quic_datagram&&) = delete;

    ngtcp2_path_storage path_{};
    std::uint32_t ecn_{NGTCP2_ECN_NOT_ECT};
    std::pmr::vector<std::byte> bytes_;
};

// One QUIC v1 + HTTP/3 connection. ngtcp2 owns packet, loss recovery and flow
// control; nghttp3 owns QPACK and HTTP stream framing. The caller owns UDP and
// TLS. Input datagrams must remain alive until next_event() drains their events.
// Events can also be published while writing (a stream closing on an
// acknowledgement), so the owner drains next_event() after every
// receive_datagram(), next_datagram() and handle_expiry().
class http3_quic_connection final {
public:
    [[nodiscard]] static http3_quic_connection client(
        const http3_quic_connection_options& options);
    [[nodiscard]] static http3_quic_connection server(
        const http3_quic_connection_options& options);

    ~http3_quic_connection();
    http3_quic_connection(const http3_quic_connection&) = delete;
    http3_quic_connection& operator=(const http3_quic_connection&) = delete;
    http3_quic_connection(http3_quic_connection&&) noexcept;
    http3_quic_connection& operator=(http3_quic_connection&&) noexcept;

    [[nodiscard]] http3_role role() const noexcept;
    [[nodiscard]] ngtcp2_conn* native_handle() noexcept;
    [[nodiscard]] const ngtcp2_conn* native_handle() const noexcept;
    [[nodiscard]] void* callback_user_data() const noexcept;
    void set_tls_native_handle(void* tls_native_handle) noexcept;
    [[nodiscard]] bool handshake_completed() const noexcept;

    // Opens and binds the local HTTP/3 control and QPACK streams after TLS has
    // established application keys. Safe to retry only when it would block.
    [[nodiscard]] http3_quic_status start_http3();
    [[nodiscard]] bool http3_started() const noexcept;
    [[nodiscard]] std::optional<std::int64_t> open_request_stream();

    // Restore authenticated transport parameters and peer SETTINGS from a
    // ticket before opening 0-RTT streams. TLS must enable early data too.
    [[nodiscard]] http3_quic_status start_early_http3(
        std::span<const std::byte> transport_parameters, http3_peer_settings settings);
    // On TLS early rejection, discard the rejected HTTP/3 streams before a
    // caller explicitly retries requests in 1-RTT; never replays requests.
    [[nodiscard]] http3_quic_status reject_early_data();
    // Client only, QUIC v1: validate a new path before switching. The caller
    // must send packets on output.path_ and keep both paths receiving.
    [[nodiscard]] http3_quic_result migrate(const ngtcp2_path& path, ngtcp2_tstamp timestamp);
    // Payload follows the HTTP Datagram Quarter Stream ID. Owned and bounded;
    // invalid_state means unsupported/oversized, would_block means queue full.
    [[nodiscard]] http3_quic_status submit_datagram(
        std::int64_t stream_id, std::span<const std::byte> payload);
    [[nodiscard]] std::size_t max_datagram_payload_size(std::int64_t stream_id) const noexcept;
    [[nodiscard]] http3_quic_result receive_datagram(const ngtcp2_path& path,
        std::uint32_t ecn, std::span<const std::byte> bytes,
        ngtcp2_tstamp timestamp);
    // While the connection is open: the next packet, or would_block. After a
    // local close: the CONNECTION_CLOSE packet once, then closed.
    [[nodiscard]] http3_quic_result next_datagram(std::size_t capacity,
        ngtcp2_tstamp timestamp, http3_quic_datagram& output);
    [[nodiscard]] http3_quic_result handle_expiry(ngtcp2_tstamp timestamp);
    [[nodiscard]] ngtcp2_tstamp expiry() const noexcept;

    // Returns receive flow-control credit for DATA payload the application
    // consumed (RFC 9000 §4.1). nghttp3 credits framing bytes itself.
    void consume_stream_data(std::int64_t stream_id, std::size_t bytes) noexcept;
    // Abandons both directions of a request stream with RESET_STREAM and
    // STOP_SENDING carrying `application_error_code` (RFC 9114 §4.1.1).
    void reset_stream(std::int64_t stream_id, std::uint64_t application_error_code) noexcept;
    // Stops reading a request whose response no longer needs the rest of its
    // content (RFC 9114 §4.1.2 permits H3_NO_ERROR once the response is
    // complete).
    void stop_receiving(std::int64_t stream_id, std::uint64_t application_error_code) noexcept;
    // Requests an orderly close: the next next_datagram() writes a
    // CONNECTION_CLOSE carrying this application error code (RFC 9000
    // §10.2; H3_NO_ERROR for a graceful HTTP/3 shutdown, RFC 9114 §5.2).
    void close(std::uint64_t application_error_code) noexcept;
    // The closing or draining period began (see http3_quic_status::closed).
    [[nodiscard]] bool closed() const noexcept;
    // How long a closed connection keeps absorbing late packets: three times
    // the current probe timeout (RFC 9000 §10.2).
    [[nodiscard]] ngtcp2_duration close_period() const noexcept;

    [[nodiscard]] std::optional<http3_event> next_event();
    [[nodiscard]] http3_connection& http3() noexcept;
    [[nodiscard]] const http3_connection& http3() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> connection_error() const noexcept;

private:
    class impl;
    explicit http3_quic_connection(http3_role role,
        const http3_quic_connection_options& options);
    std::unique_ptr<impl> impl_;
};

}  // namespace ruvia
