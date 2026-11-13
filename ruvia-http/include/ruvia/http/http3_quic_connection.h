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

// The error code of the CONNECTION_CLOSE that ended the connection, whether
// sent or received (RFC 9000 §19.19): transport_error and a transport close
// carry a QUIC transport error code (§20.1, 0x0100-0x01ff for a TLS alert);
// application_error and an application close carry an HTTP/3 code. Both are
// unset while the connection is open, and after an idle timeout or a
// stateless reset.
struct http3_quic_result final {
    http3_quic_status status_{http3_quic_status::accepted};
    std::optional<std::uint64_t> transport_error_code_{};
    std::optional<http3_error_code> application_error_code_{};
};

using http3_quic_stateless_reset_token =
    std::array<std::uint8_t, NGTCP2_STATELESS_RESET_TOKENLEN>;

// The QUIC-TLS adapter (for example ngtcp2_crypto_ossl's ngtcp2_crypto_*_cb).
// The connection installs these unchanged; they receive the connection's own
// user data, which the adapters do not read. client_initial_ and recv_retry_
// are client-only, recv_client_initial_ server-only; the others are required
// for both roles. The connection owns every stream, acknowledgement and
// close callback ngtcp2 defines, so none can be supplied.
struct http3_quic_crypto_callbacks final {
    ngtcp2_client_initial client_initial_{nullptr};
    ngtcp2_recv_client_initial recv_client_initial_{nullptr};
    ngtcp2_recv_crypto_data recv_crypto_data_{nullptr};
    ngtcp2_encrypt encrypt_{nullptr};
    ngtcp2_decrypt decrypt_{nullptr};
    ngtcp2_hp_mask hp_mask_{nullptr};
    ngtcp2_recv_retry recv_retry_{nullptr};
    ngtcp2_update_key update_key_{nullptr};
    ngtcp2_delete_crypto_aead_ctx delete_crypto_aead_ctx_{nullptr};
    ngtcp2_delete_crypto_cipher_ctx delete_crypto_cipher_ctx_{nullptr};
    ngtcp2_get_path_challenge_data2 get_path_challenge_data_{nullptr};
    ngtcp2_version_negotiation version_negotiation_{nullptr};
    ngtcp2_rand rand_{nullptr};
};

// Connection lifecycle notifications for the owner; each, when set, receives
// http3_quic_connection_options::callback_user_data_. A nonzero return fails
// the connection as ngtcp2 documents for the callback.
struct http3_quic_owner_callbacks final {
    // Issues a connection ID and its stateless reset token (RFC 9000 §5.1.1).
    ngtcp2_get_new_connection_id2 get_new_connection_id_{nullptr};
    ngtcp2_remove_connection_id remove_connection_id_{nullptr};
    ngtcp2_handshake_completed handshake_completed_{nullptr};
    ngtcp2_handshake_confirmed handshake_confirmed_{nullptr};
    ngtcp2_recv_stateless_reset2 recv_stateless_reset_{nullptr};
    // Runs after the connection discarded its rejected 0-RTT HTTP/3 streams.
    ngtcp2_tls_early_data_rejected tls_early_data_rejected_{nullptr};
};

struct http3_quic_connection_options final {
    http3_quic_crypto_callbacks crypto_callbacks_{};
    http3_quic_owner_callbacks owner_callbacks_{};
    ngtcp2_cid source_connection_id_{};
    ngtcp2_cid destination_connection_id_{};
    // Server only and required (RFC 9000 §7.3): the Destination Connection ID
    // of the client's first Initial packet (extracted from the Retry token
    // when Retry was used).
    std::optional<ngtcp2_cid> original_destination_connection_id_{};
    // Server only: the Source Connection ID sent in the Retry packet that the
    // client echoed as the Destination Connection ID of its second Initial
    // (RFC 9000 §7.3, §18.2). Requires retry_token_ to be non-empty.
    std::optional<ngtcp2_cid> retry_source_connection_id_{};
    // Server only: the validated Retry token from the client's second Initial
    // (RFC 9000 §8.1.2). Lifts the anti-amplification limit in ngtcp2.
    borrowed_span<std::uint8_t> retry_token_{};
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
    // Leave spare CIDs for a new candidate while the current path and an
    // old-path validation coexist (RFC 9000 §5.1.1).
    std::uint64_t active_connection_id_limit_{4};
    std::size_t max_udp_payload_size_{1350};
    // Bounded unreliable output retained until ngtcp2 accepts each frame.
    std::size_t max_pending_datagram_bytes_{64 * 1024};
    void* callback_user_data_{nullptr};
    std::pmr::memory_resource* resource_{nullptr};
    // The connection installs its own received_data_credit_sink_: a returned
    // http3_received_data_credit extends the stream and connection receive
    // windows (RFC 9000 §4.1), so DATA credit returns only as the application
    // consumes it. Framing bytes are credited at once.
    http3_connection_options http3_{};
};

// Socket addresses of one QUIC path (RFC 9000 §9), in the platform's
// sockaddr layout.
struct http3_quic_path final {
    std::span<const std::byte> local_;
    std::span<const std::byte> remote_;
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
    // For the QUIC-TLS adapter's connection reference
    // (ngtcp2_crypto_conn_ref::get_conn); every other capability has a member.
    [[nodiscard]] ngtcp2_conn* native_handle() noexcept;
    [[nodiscard]] const ngtcp2_conn* native_handle() const noexcept;
    [[nodiscard]] void* callback_user_data() const noexcept;
    void set_tls_native_handle(void* tls_native_handle) noexcept;
    [[nodiscard]] bool handshake_completed() const noexcept;
    [[nodiscard]] bool after_retry() const noexcept;
    [[nodiscard]] bool stateless_reset_received() const noexcept;

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
    // Client: TLS rejected the early data, whether reported through
    // reject_early_data() or by the QUIC-TLS adapter during the handshake.
    [[nodiscard]] bool early_data_rejected() const noexcept;
    // The transport parameters 0-RTT depends on (RFC 9000 §7.4.1). A client
    // gets the server's, once received, to store with the TLS session ticket
    // and restore with start_early_http3(); a server gets its own, which a
    // ticket's resumption check must bind. Returns the encoded size, or
    // nullopt when a client has not received them yet or `output` is too
    // small.
    [[nodiscard]] std::optional<std::size_t> encode_early_transport_parameters(
        std::span<std::byte> output) const noexcept;
    // The path the connection currently sends on, after any validated
    // migration (RFC 9000 §9). The views borrow connection storage until the
    // next call that processes input, writes output or migrates.
    [[nodiscard]] http3_quic_path current_path() const& noexcept RUVIA_LIFETIMEBOUND;
    http3_quic_path current_path() const&& = delete;
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

    // Abandons both directions of a request stream with RESET_STREAM and
    // STOP_SENDING carrying `error` (RFC 9114 §4.1.1).
    void reset_stream(std::int64_t stream_id, http3_error_code error) noexcept;
    // Ends only the sending half of a stream with RESET_STREAM carrying
    // `error`; its receiving half keeps delivering (RFC 9000 §3.1,
    // RFC 9114 §4.1).
    void reset_send_stream(std::int64_t stream_id, http3_error_code error) noexcept;
    // Stops reading a request whose response no longer needs the rest of its
    // content (RFC 9114 §4.1.2 permits H3_NO_ERROR once the response is
    // complete).
    void stop_receiving(std::int64_t stream_id, http3_error_code error) noexcept;
    // Requests an orderly close: the next next_datagram() writes a
    // CONNECTION_CLOSE carrying `error` (RFC 9000 §10.2; H3_NO_ERROR for a
    // graceful HTTP/3 shutdown, RFC 9114 §5.2).
    void close(http3_error_code error) noexcept;
    // The closing or draining period began (see http3_quic_status::closed).
    [[nodiscard]] bool closed() const noexcept;
    // How long a closed connection keeps absorbing late packets: three times
    // the current probe timeout (RFC 9000 §10.2).
    [[nodiscard]] ngtcp2_duration close_period() const noexcept;

    [[nodiscard]] std::optional<http3_event> next_event();
    [[nodiscard]] http3_connection& http3() noexcept;
    [[nodiscard]] const http3_connection& http3() const noexcept;
    // The HTTP/3 code of the CONNECTION_CLOSE this connection sent or
    // received, or of the codec error it failed on.
    [[nodiscard]] std::optional<http3_error_code> application_error() const noexcept;
    // The QUIC transport error code (RFC 9000 §20.1) of the CONNECTION_CLOSE
    // this connection sent or received.
    [[nodiscard]] std::optional<std::uint64_t> transport_error() const noexcept;

private:
    class impl;
    explicit http3_quic_connection(http3_role role,
        const http3_quic_connection_options& options);
    std::unique_ptr<impl, detail::http_pmr_object_deleter<impl>> impl_;
};

// Writes a QUIC Stateless Reset packet (RFC 9000 §10.3) into `output`, using
// at most `max_packet_bytes` (pass `trigger_datagram_bytes - 1` so the packet
// is strictly smaller than the datagram that triggered it, RFC 9000 §10.3.3).
// Returns the number of bytes written, or 0 when `output`, `max_packet_bytes`
// or `random_bytes` cannot form a valid Stateless Reset.
[[nodiscard]] std::size_t write_http3_quic_stateless_reset(
    const http3_quic_stateless_reset_token& token,
    std::span<const std::uint8_t> random_bytes,
    std::span<std::byte> output,
    std::size_t max_packet_bytes) noexcept;

}  // namespace ruvia
