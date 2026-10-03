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

#include "ruvia/http/Http3Settings.h"
#include "ruvia/http/quic_types.h"
#include "ruvia/web/detail/client/ClientTransport.h"

namespace ruvia::detail {

// Owns the preconfigured TLS context used by outbound QUIC connections.
class http3_quic_client_tls_context final {
public:
    struct ssl_session_deleter {
        void operator()(SSL_SESSION* session) const noexcept {
            SSL_SESSION_free(session);
        }
    };
    using ssl_session_owner = std::unique_ptr<SSL_SESSION, ssl_session_deleter>;

    struct ticket_lease final {
        ticket_lease(ssl_session_owner session, std::pmr::string host,
            std::uint64_t identity_generation, ruvia::quic_version version,
            std::pmr::vector<std::byte> transport_parameters,
            std::optional<ruvia::Http3Settings> settings)
            : session(std::move(session)),
              host(std::move(host)),
              identity_generation(identity_generation),
              version(version),
              transport_parameters(std::move(transport_parameters)),
              settings(std::move(settings)) {}
        ticket_lease(const ticket_lease&) = delete;
        ticket_lease& operator=(const ticket_lease&) = delete;
        ticket_lease(ticket_lease&&) noexcept = default;

        ssl_session_owner session;
        std::pmr::string host;
        std::uint64_t identity_generation{};
        ruvia::quic_version version{};
        std::pmr::vector<std::byte> transport_parameters;
        std::optional<ruvia::Http3Settings> settings;
    };

    // The memory resource must outlive this context; host normalization is temporary.
    explicit http3_quic_client_tls_context(ClientTransportConfigView config,
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
        std::optional<ruvia::Http3Settings> settings);

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
