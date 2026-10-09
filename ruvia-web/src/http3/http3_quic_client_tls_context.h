#pragma once

#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/ssl.h>

#include "ruvia/http/http3_settings.h"
#include "ruvia/http/quic_types.h"

#include "client/client_transport.h"

namespace ruvia::detail {

// Owns the preconfigured TLS context used by outbound QUIC connections.
class http3_quic_client_tls_context final {
public:
    struct ssl_session_deleter {
        void operator()(SSL_SESSION* session_value) const noexcept {
            SSL_SESSION_free(session_value);
        }
    };
    using ssl_session_owner = std::unique_ptr<SSL_SESSION, ssl_session_deleter>;

    struct ticket_lease final {
        ticket_lease(ssl_session_owner session_value, std::pmr::string host,
            std::uint64_t identity_generation, ruvia::quic_version version,
            std::pmr::vector<std::byte> transport_parameters,
            std::optional<ruvia::http3_settings> settings)
            : session_(std::move(session_value)),
              host_(std::move(host)),
              identity_generation_(identity_generation),
              version_(version),
              transport_parameters_(std::move(transport_parameters)),
              settings_(std::move(settings)) {}
        ticket_lease(const ticket_lease&) = delete;
        ticket_lease& operator=(const ticket_lease&) = delete;
        ticket_lease(ticket_lease&&) noexcept = default;

        ssl_session_owner session_;
        std::pmr::string host_;
        std::uint64_t identity_generation_{};
        ruvia::quic_version version_{};
        std::pmr::vector<std::byte> transport_parameters_;
        std::optional<ruvia::http3_settings> settings_;
    };

    // The memory resource must outlive this context; host normalization is temporary.
    explicit http3_quic_client_tls_context(client_transport_config_view config,
        std::pmr::memory_resource* resource = std::pmr::get_default_resource());
    ~http3_quic_client_tls_context();

    http3_quic_client_tls_context(const http3_quic_client_tls_context&) = delete;
    http3_quic_client_tls_context& operator=(const http3_quic_client_tls_context&) = delete;
    http3_quic_client_tls_context(http3_quic_client_tls_context&&) = delete;
    http3_quic_client_tls_context& operator=(http3_quic_client_tls_context&&) = delete;

    [[nodiscard]] SSL_CTX* native_handle() const noexcept;
    [[nodiscard]] std::optional<ticket_lease> take_ticket(
        std::string_view host, ruvia::quic_version version);
    void remember_ticket(SSL_SESSION* session, std::string_view host,
        ruvia::quic_version version, std::span<const std::byte> transport_parameters,
        std::optional<ruvia::http3_settings> settings);

    // Configures a single connection's h3 ALPN and peer identity checks.
    // The SSL object must have been created from this context.
    void prepare(SSL* ssl, std::string_view host) const;

private:
    struct context_deleter {
        void operator()(SSL_CTX* context) const noexcept;
    };
    std::unique_ptr<SSL_CTX, context_deleter> context_;
    std::pmr::memory_resource* resource_;
    std::optional<ticket_lease> ticket_;
    std::uint64_t identity_generation_{};
};

}  // namespace ruvia::detail
