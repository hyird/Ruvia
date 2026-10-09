#include "http3/Http3QuicClientTlsContext.h"

#include <array>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "ruvia/core/DnsHost.h"

namespace ruvia::detail {
namespace {

std::atomic<std::uint64_t> next_identity_generation{1};

std::pmr::string canonical_host(std::string_view host, std::pmr::memory_resource* resource) {
    validateClientOriginHost(host, "client TLS host is empty", "client TLS host is invalid");
    std::pmr::string result(host, resource);
    if (!isClientIpAddress(result) && result.ends_with('.')) {
        result.pop_back();
    }
    return result;
}

bool supported_version(ruvia::quic_version version) noexcept {
    return version == ruvia::quic_version::v1 || version == ruvia::quic_version::v2;
}

constexpr std::size_t max_ticket_size = 16 * 1024;
constexpr std::size_t max_transport_parameters_size = 64 * 1024;

bool expired(const SSL_SESSION* session) noexcept {
    const auto issued = SSL_SESSION_get_time_ex(session);
    const auto timeout = SSL_SESSION_get_timeout(session);
    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    return issued <= 0 || timeout <= 0 || issued > now || now - issued >= timeout;
}

}  // namespace

http3_quic_client_tls_context::http3_quic_client_tls_context(ClientTransportConfigView config,
    std::pmr::memory_resource* resource)
    : resource_(resource ? resource : std::pmr::get_default_resource()),
      identity_generation_(next_identity_generation.fetch_add(1, std::memory_order_relaxed)) {
    if (identity_generation_ == 0) {
        identity_generation_ = next_identity_generation.fetch_add(1, std::memory_order_relaxed);
    }
    context_.reset(SSL_CTX_new(TLS_method()));
    if (!context_) {
        throw std::runtime_error("failed to create QUIC client TLS context");
    }
    configure_client_tls_context(*context_, config, client_tls_protocol::quic);
}

http3_quic_client_tls_context::~http3_quic_client_tls_context() = default;

SSL_CTX* http3_quic_client_tls_context::native_handle() const noexcept {
    return context_.get();
}

std::optional<http3_quic_client_tls_context::ticket_lease>
http3_quic_client_tls_context::take_ticket(std::string_view host, ruvia::quic_version version) {
    if (!ticket_) {
        return std::nullopt;
    }
    auto key_host = canonical_host(host, resource_);
    if (ticket_->host != key_host || ticket_->identity_generation != identity_generation_ ||
        ticket_->version != version || !supported_version(version) ||
        !ticket_->session || SSL_SESSION_is_resumable(ticket_->session.get()) != 1 ||
        expired(ticket_->session.get())) {
        ticket_.reset();
        return std::nullopt;
    }
    std::optional<ticket_lease> result(std::move(ticket_));
    ticket_.reset();
    return result;
}

void http3_quic_client_tls_context::remember_ticket(SSL_SESSION* session,
    std::string_view host, ruvia::quic_version version,
    std::span<const std::byte> transport_parameters,
    std::optional<ruvia::Http3Settings> settings) {
    if (!session || SSL_SESSION_is_resumable(session) != 1 || !supported_version(version) ||
        transport_parameters.size() > max_transport_parameters_size || expired(session)) {
        return;
    }
    const unsigned char* ticket{};
    std::size_t ticket_size{};
    SSL_SESSION_get0_ticket(session, &ticket, &ticket_size);
    if (!ticket || ticket_size == 0 || ticket_size > max_ticket_size) {
        return;
    }
    auto owned_host = canonical_host(host, resource_);
    ssl_session_owner owned_session(SSL_SESSION_dup(session));
    if (!owned_session) {
        return;
    }
    std::pmr::vector<std::byte> owned_parameters(transport_parameters.begin(),
        transport_parameters.end(), resource_);
    ticket_.emplace(std::move(owned_session), std::move(owned_host), identity_generation_, version,
        std::move(owned_parameters), std::move(settings));
}

void http3_quic_client_tls_context::prepare(SSL* ssl, std::string_view host) const {
    if (ssl == nullptr || SSL_get_SSL_CTX(ssl) != context_.get()) {
        throw std::invalid_argument("SSL connection does not use this QUIC client TLS context");
    }
    validateClientOriginHost(host, "client TLS host is empty", "client TLS host is invalid");

    const bool ip_address = isClientIpAddress(host);
    std::pmr::string normalized(host, resource_);
    if (!ip_address && normalized.ends_with('.')) {
        normalized.pop_back();
    }
    if (!ip_address && SSL_set_tlsext_host_name(ssl, normalized.c_str()) != 1) {
        throw std::runtime_error("failed to set client TLS SNI host");
    }

    if (SSL_set_alpn_protos(ssl, std::array<unsigned char, 3>{2, 'h', '3'}.data(), 3) != 0) {
        throw std::runtime_error("failed to configure h3 ALPN");
    }

    if (SSL_CTX_get_verify_mode(context_.get()) == SSL_VERIFY_PEER) {
        X509_VERIFY_PARAM* const parameters = SSL_get0_param(ssl);
        const int configured = ip_address
                                   ? X509_VERIFY_PARAM_set1_ip_asc(parameters, normalized.c_str())
                                   : SSL_set1_dnsname(ssl, normalized.c_str());
        if (configured != 1) {
            throw std::runtime_error("failed to configure client TLS peer host verification");
        }
    }
}

void http3_quic_client_tls_context::context_deleter::operator()(SSL_CTX* context) const noexcept {
    SSL_CTX_free(context);
}

}  // namespace ruvia::detail
