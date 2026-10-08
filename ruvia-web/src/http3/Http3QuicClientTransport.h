#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "ruvia/http/quic_connection.h"

#include "http3/Http3QuicClientTlsContext.h"
#include "http3/openssl_quic_crypto_provider.h"
#include "http3/openssl_quic_tls_session.h"

namespace ruvia::detail {

// Worker-affine owner of one HTTP-core QUIC client connection and its OpenSSL TLS
// driver. The TLS context and resource outlive this adapter.
class http3_quic_client_transport final {
public:
    http3_quic_client_transport(http3_quic_client_tls_context& tls,
        ruvia::quic_connection_config config, std::string_view host,
        ruvia::quic_timestamp now,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource(),
        bool enable_early_data = false);
    ~http3_quic_client_transport() noexcept;
    http3_quic_client_transport(const http3_quic_client_transport&) = delete;
    http3_quic_client_transport& operator=(const http3_quic_client_transport&) = delete;
    http3_quic_client_transport(http3_quic_client_transport&&) = delete;
    http3_quic_client_transport& operator=(http3_quic_client_transport&&) = delete;

    [[nodiscard]] ruvia::quic_connection& connection() noexcept {
        return *connection_;
    }
    [[nodiscard]] const ruvia::quic_connection& connection() const noexcept {
        return *connection_;
    }
    [[nodiscard]] ruvia::quic_operation_status receive(
        const ruvia::quic_datagram_view& datagram, ruvia::quic_timestamp now);
    [[nodiscard]] ruvia::quic_packet_result write_packet(
        std::span<std::byte> output, ruvia::quic_timestamp now);
    [[nodiscard]] std::optional<ruvia::quic_timestamp> next_expiry() const noexcept;
    [[nodiscard]] ruvia::quic_operation_status handle_expiry(ruvia::quic_timestamp now);
    void remember_resumption_ticket(std::optional<ruvia::Http3Settings> settings);
    [[nodiscard]] bool early_data_enabled() const noexcept {
        return early_data_enabled_;
    }

    void stop_tls() noexcept;
    void close() noexcept;

private:
    std::pmr::memory_resource* resource_;
    http3_quic_client_tls_context* tls_;
    std::pmr::string host_;
    openssl_quic_crypto_provider crypto_;
    bool early_data_enabled_{};
    std::optional<openssl_quic_tls_session> tls_session_;
    std::optional<ruvia::quic_connection> connection_;
};

}  // namespace ruvia::detail
