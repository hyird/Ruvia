#pragma once

#include <cstdint>
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

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/server_config.h"

#include "tls/tls_credential.h"
#include "tls/tls_session_ticket_keys.h"

namespace ruvia::detail {

struct http_server_listener_definition final {
    // Every TLS member is loaded when the listener is configured: no file name
    // or private-key password survives in a definition or its clones.
    struct tls_client_certificate_policy_type final {
        tls_client_certificate_requirement requirement_{tls_client_certificate_requirement::optional};
        tls_trust_store client_ca_;
    };

    struct tls_type final {
        struct sni_identity_type final {
            sni_identity_type(std::string_view host, tls_certificate certificate,
                std::pmr::memory_resource* resource)
                : host_(host, pmr_resource_or_default(resource)),
                  certificate_(std::move(certificate)) {}

            std::pmr::string host_;
            tls_certificate certificate_;
        };

        tls_type(tls_certificate certificate, std::pmr::memory_resource* resource)
            : certificate_(std::move(certificate)),
              sni_identities_(pmr_resource_or_default(resource)) {}

        tls_certificate certificate_;
        std::optional<tls_client_certificate_policy_type> client_certificates_;
        std::pmr::vector<sni_identity_type> sni_identities_;
        // Generated once by server configuration validation and shared
        // read-only by every worker's TLS contexts.
        std::optional<tls_session_ticket_keys> session_ticket_keys_;
    };

    struct plain_http_type final {};

    struct redirect_http_to_https_type final {
        std::uint16_t https_port_;
    };

    using transport_type = std::variant<plain_http_type, tls_type, redirect_http_to_https_type>;

    http_server_listener_definition(asio::ip::tcp::endpoint configured_endpoint,
        transport_type configured_transport = plain_http_type{})
        : endpoint_(std::move(configured_endpoint)),
          transport_(std::move(configured_transport)) {}

    [[nodiscard]] http_server_listener_definition clone(std::pmr::memory_resource* resource) const;

    asio::ip::tcp::endpoint endpoint_;
    transport_type transport_;
};

// Every TLS context of a listener uses the configuration's one ticket key set.
// Only validated configurations carry keys; an unvalidated definition must
// never reach a serving TLS context.
inline void install_tls_session_ticket_keys(
    SSL_CTX& context, const http_server_listener_definition::tls_type& tls) {
    if (!tls.session_ticket_keys_.has_value()) {
        throw std::logic_error("TLS listener session ticket keys were not generated");
    }
    tls.session_ticket_keys_->install(context);
}

using sni_context_store_type = std::pmr::vector<asio::ssl::context>;
using sni_context_lookup_type = std::pmr::vector<std::pair<std::pmr::string, asio::ssl::context*>>;

class http_server_session_config final {
public:
    explicit http_server_session_config(const http_server_listener_definition& definition,
        std::pmr::memory_resource* resource);

    http_server_session_config(const http_server_session_config&) = delete;
    http_server_session_config& operator=(const http_server_session_config&) = delete;
    http_server_session_config(http_server_session_config&&) = delete;
    http_server_session_config& operator=(http_server_session_config&&) = delete;

    [[nodiscard]] const http_server_listener_definition::tls_type* tls() const& noexcept {
        return std::get_if<http_server_listener_definition::tls_type>(&transport_);
    }
    const http_server_listener_definition::tls_type* tls() const&& = delete;

    [[nodiscard]] const http_server_listener_definition::redirect_http_to_https_type* redirect()
        const& noexcept {
        return std::get_if<http_server_listener_definition::redirect_http_to_https_type>(&transport_);
    }
    const http_server_listener_definition::redirect_http_to_https_type* redirect() const&& = delete;

    http_server_listener_definition::transport_type transport_;
    std::optional<asio::ssl::context> tls_context_;
    sni_context_store_type sni_contexts_;
    sni_context_lookup_type sni_lookup_;
};

}  // namespace ruvia::detail
