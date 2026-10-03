#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>

#include "ruvia/http/quic_tls_handshake.h"

namespace ruvia::detail {
class quic_connection_state;
struct quic_cid_registry_view;
struct quic_connection_state_deleter {
    std::pmr::memory_resource* resource{};
    void operator()(quic_connection_state* state) const noexcept;
};
}  // namespace ruvia::detail

namespace ruvia {

struct quic_stream_open_result {
    quic_operation_status status{quic_operation_status::would_block};
    std::uint64_t stream_id{};
};

// One owner-thread connection. It is address-stable and non-movable for the full
// lifetime of the TLS driver borrow. Packet/stream/datagram buffers are always supplied
// by the caller; accepted stream/datagram input is copied into bounded connection storage.
class quic_connection {
public:
    quic_connection(quic_connection_config config, quic_crypto_provider_view crypto,
        quic_tls_driver_view tls_driver, std::pmr::memory_resource* resource, quic_timestamp now,
        std::span<const std::byte> early_transport_parameters = {});
    quic_connection(const quic_connection&) = delete;
    quic_connection& operator=(const quic_connection&) = delete;
    quic_connection(quic_connection&&) = delete;
    quic_connection& operator=(quic_connection&&) = delete;
    ~quic_connection() noexcept;

    quic_tls_handshake& tls_handshake() noexcept;
    quic_connection_info info() const noexcept;
    // Encodes the server parameters currently negotiated on this connection for ticket storage.
    std::size_t encode_early_transport_parameters(std::span<std::byte> output) const;
    // Starts one client-only migration to a bound local address. A started
    // migration remains active until path validation completes or fails.
    [[nodiscard]] quic_path_migration start_path_migration(const quic_address& local_address);
    [[nodiscard]] std::optional<quic_path_migration> path_migration(
        std::uint64_t id) const noexcept;
    // ngtcp2 cannot cancel path validation in place; aborting closes the QUIC
    // connection. Its owner must still drive and join normal connection teardown.
    [[nodiscard]] quic_operation_status cancel_path_migration(std::uint64_t id);
    // Records a local candidate-path I/O failure without closing the active path.
    [[nodiscard]] quic_operation_status fail_path_migration(std::uint64_t id) noexcept;
    quic_operation_status receive(const quic_datagram_view& datagram, quic_timestamp now);
    quic_packet_result write_packet(std::span<std::byte> output, quic_timestamp now);
    std::optional<quic_timestamp> next_expiry() const noexcept;
    quic_operation_status handle_expiry(quic_timestamp now);
    quic_operation_status update_key(quic_timestamp now);

    quic_stream_open_result open_stream(bool unidirectional);
    quic_stream_accept_batch accept_streams() noexcept;
    quic_stream_read_result read_stream(std::uint64_t stream_id, std::span<std::byte> output);
    [[nodiscard]] quic_stream_info stream_info(std::uint64_t stream_id) const noexcept;
    // Returns and removes QUIC stream IDs invalidated by a rejected 0-RTT
    // attempt. The owning HTTP layer must rebuild its per-stream protocol state.
    [[nodiscard]] std::size_t take_rejected_early_streams(
        std::span<std::uint64_t> output) noexcept;
    quic_stream_read_result read_health(std::uint64_t stream_id) const noexcept;
    // accepted means the accepted prefix was copied before return; would_block retains none.
    quic_stream_write_result write_stream(std::uint64_t stream_id,
        std::span<const std::byte> input, bool fin = false);
    quic_operation_status finish_stream(std::uint64_t stream_id);
    quic_operation_status write_health(std::uint64_t stream_id) const noexcept;
    quic_operation_status reset_stream(std::uint64_t stream_id, std::uint64_t application_error);
    quic_operation_status stop_sending(std::uint64_t stream_id, std::uint64_t application_error);
    quic_operation_status terminate_bidirectional_stream(std::uint64_t stream_id,
        std::uint64_t application_error);
    quic_operation_status retire_completed_stream(std::uint64_t stream_id);
    quic_operation_status close_stream(std::uint64_t stream_id);

    std::size_t max_datagram_payload_size() const noexcept;
    quic_datagram_write_status write_datagram(std::span<const std::byte> payload);
    quic_datagram_result read_datagram(std::span<std::byte> output) noexcept;
    // A failed connection can only be closed or retired. Closing permits the
    // owner to send CONNECTION_CLOSE after catching an operation failure.
    quic_operation_status close(quic_close_reason_view reason);

private:
    friend class quic_server;
    quic_connection(quic_initial_offer offer, quic_connection_config config,
        quic_crypto_provider_view crypto, quic_tls_driver_view tls_driver,
        std::pmr::memory_resource* resource, quic_timestamp now);
    quic_operation_status retire_from_server() noexcept;
    void bind_server_cid_registry(detail::quic_cid_registry_view registry);
    std::unique_ptr<detail::quic_connection_state, detail::quic_connection_state_deleter> impl_{};
};

}  // namespace ruvia
