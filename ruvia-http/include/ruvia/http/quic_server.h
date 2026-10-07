#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>

#include "ruvia/http/quic_connection.h"

namespace ruvia {

struct quic_server_config {
    // Preferred version for compatible version negotiation; incoming offer versions are retained per connection.
    quic_version version{quic_version::v1};
    quic_transport_parameters local_transport_parameters{};
    quic_limits limits{};
    quic_cid_partition cid_partition{};
    std::size_t max_active_connections{4096};
    std::size_t max_pending_connections{256};
    std::size_t max_pending_datagram_bytes{1U << 20};
};

struct quic_server_admit_result {
    quic_operation_status status{quic_operation_status::would_block};
    quic_connection_token connection{};
};

// Stateless destination-CID routing using the same header parser as quic_server.
// Client-chosen Initial DCIDs select a worker once; all server-issued CIDs keep
// that worker across CID rotation and address migration. Invalid headers or a
// zero partition count return nullopt. Short headers use the server's 16-byte CID.
[[nodiscard]] std::optional<std::uint32_t> quic_datagram_partition(
    std::span<const std::byte> packet, std::uint32_t partition_count) noexcept;

// Sans-I/O Initial classifier, CID router, and owner of pending/admitted protocol
// connections. Stateless partition routing and route_datagram share HTTP-owned
// header decoding; Web never decodes CIDs. Each admission retains its TLS borrow.
class quic_server {
public:
    quic_server(quic_server_config config, quic_crypto_provider_view crypto,
        std::pmr::memory_resource* resource);
    quic_server(const quic_server&) = delete;
    quic_server& operator=(const quic_server&) = delete;
    quic_server(quic_server&&) = delete;
    quic_server& operator=(quic_server&&) = delete;
    ~quic_server() noexcept;

    // The datagram bytes are borrowed for this call. An initial_offer is a bounded
    // value and remains valid independently of the input datagram.
    quic_server_route route_datagram(const quic_datagram_view& datagram);
    quic_packet_result write_version_negotiation(quic_version_negotiation_plan& plan,
        std::span<std::byte> output);
    quic_server_admit_result admit_initial(const quic_initial_offer& offer,
        quic_tls_driver_view connection_tls_driver, quic_timestamp now);
    quic_connection& connection(quic_connection_token token);
    const quic_connection& connection(quic_connection_token token) const;
    quic_operation_status receive(quic_connection_token token,
        const quic_datagram_view& datagram, quic_timestamp now);
    std::optional<quic_timestamp> next_expiry() const noexcept;
    quic_operation_status handle_expiry(quic_timestamp now);
    quic_operation_status retire(quic_connection_token token) noexcept;
    std::size_t connection_count() const noexcept;
    std::size_t pending_connection_count() const noexcept;

private:
    struct impl;
    struct impl_deleter {
        std::pmr::memory_resource* resource{};
        void operator()(impl* value) const noexcept;
    };
    std::unique_ptr<impl, impl_deleter> impl_{nullptr, impl_deleter{}};
};

}  // namespace ruvia
