#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/web/server_config.h"

#include "tls/tls_credential.h"
#include "tls/tls_session_ticket_keys.h"

namespace ruvia::detail {

// A server-chosen QUIC connection ID carries its owning worker's index in two
// bytes, so packets can be steered to that worker.
inline constexpr std::size_t http3_max_worker_count = 65536;

struct http_server_listener_definition final {
    // Every TLS member is loaded when the listener is configured: no file name
    // or private-key password survives in a definition or its clones.
    struct tls_client_certificate_policy final {
        tls_client_certificate_requirement requirement_{tls_client_certificate_requirement::optional};
        tls_trust_store client_ca_;
    };

    struct tls final {
        struct sni_identity final {
            sni_identity(std::string_view host, tls_certificate certificate,
                std::pmr::memory_resource* resource)
                : host_(host, pmr_resource_or_default(resource)),
                  certificate_(std::move(certificate)) {}

            std::pmr::string host_;
            tls_certificate certificate_;
        };

        tls(tls_certificate certificate, std::pmr::memory_resource* resource)
            : certificate_(std::move(certificate)),
              sni_identities_(pmr_resource_or_default(resource)) {}

        tls_certificate certificate_;
        std::uint64_t max_outgoing_records_{std::uint64_t{1} << 23};
        std::optional<tls_client_certificate_policy> client_certificates_;
        std::pmr::vector<sni_identity> sni_identities_;
        // Created once by server configuration validation and shared by every
        // worker, which copies its keys into its own TLS contexts' key set.
        std::shared_ptr<tls_session_ticket_key_ring> session_ticket_keys_;
    };

    struct plain_http final {};

    struct redirect_http_to_https final {
        std::uint16_t https_port_;
    };

    // The HTTP/3 service of a TLS listener: a QUIC endpoint on the same
    // address and UDP port_, serving the listener's TLS identities.
    struct http3_service final {
        std::uint16_t port_;
        std::chrono::seconds alt_svc_max_age_;
        // Created once by server configuration validation and shared by every
        // worker: keys the connection IDs that steer packets between workers
        // and derives stateless reset tokens (RFC 9000 §10.3.2).
        std::shared_ptr<const std::array<std::uint8_t, 32>> secret_;
    };

    using transport_type = std::variant<plain_http, tls, redirect_http_to_https>;

    http_server_listener_definition(asio::ip::tcp::endpoint configured_endpoint,
        transport_type configured_transport = plain_http{},
        std::optional<http3_service> configured_http3 = std::nullopt)
        : endpoint_(std::move(configured_endpoint)),
          transport_(std::move(configured_transport)),
          http3_(std::move(configured_http3)) {}

    [[nodiscard]] http_server_listener_definition clone(std::pmr::memory_resource* resource) const;

    asio::ip::tcp::endpoint endpoint_;
    transport_type transport_;
    // Only a TLS listener carries one.
    std::optional<http3_service> http3_;
};

// Only validated configurations carry a key ring; an unvalidated definition
// must never reach a serving TLS context.
[[nodiscard]] inline tls_session_ticket_key_ring& tls_session_ticket_keys(
    const http_server_listener_definition::tls& tls) {
    if (tls.session_ticket_keys_ == nullptr) {
        throw std::logic_error("TLS listener session ticket keys were not generated");
    }
    return *tls.session_ticket_keys_;
}

using sni_context_store_type = std::pmr::vector<asio::ssl::context>;

class http_server_session_config;

// The SNI callback argument of one TLS context family: the listener's
// identities and the per-host contexts built for that family (TLS over TCP,
// or TLS for QUIC).
struct http_server_sni_selection final {
    const http_server_session_config* session_;
    sni_context_store_type* contexts_;
};

class http_server_session_config final {
public:
    explicit http_server_session_config(const http_server_listener_definition& definition,
        std::pmr::memory_resource* resource);

    http_server_session_config(const http_server_session_config&) = delete;
    http_server_session_config& operator=(const http_server_session_config&) = delete;
    http_server_session_config(http_server_session_config&&) = delete;
    http_server_session_config& operator=(http_server_session_config&&) = delete;

    [[nodiscard]] const http_server_listener_definition::tls* tls() const& noexcept {
        return std::get_if<http_server_listener_definition::tls>(&transport_);
    }
    const http_server_listener_definition::tls* tls() const&& = delete;

    [[nodiscard]] const http_server_listener_definition::redirect_http_to_https* redirect()
        const& noexcept {
        return std::get_if<http_server_listener_definition::redirect_http_to_https>(&transport_);
    }
    const http_server_listener_definition::redirect_http_to_https* redirect() const&& = delete;

    http_server_listener_definition::transport_type transport_;
    std::optional<http_server_listener_definition::http3_service> http3_;
    // The Alt-Svc field value advertising http3_ (RFC 7838 §3), sent on every
    // response of this TLS listener; empty without an HTTP/3 service.
    std::pmr::string alternative_service_;
    // This worker's copy of the listener's ticket keys, refreshed on the
    // worker thread once per key epoch. The TLS contexts below point at it, so
    // it is declared before them and outlives them.
    std::optional<tls_session_ticket_key_set> ticket_keys_;
    std::optional<asio::ssl::context> tls_context_;
    sni_context_store_type sni_contexts_;
    // TLS 1.3 contexts with ALPN h3 for the HTTP/3 service, same identities.
    std::optional<asio::ssl::context> quic_tls_context_;
    sni_context_store_type quic_sni_contexts_;
    http_server_sni_selection tls_sni_{this, &sni_contexts_};
    http_server_sni_selection quic_sni_{this, &quic_sni_contexts_};
};

}  // namespace ruvia::detail
