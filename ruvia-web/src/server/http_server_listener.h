#pragma once

#include <cstdint>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <asio/ip/tcp.hpp>
#include <asio/ssl/context.hpp>

#include "ruvia/core/memory/pmr_resource.h"
#include "ruvia/web/server_config.h"

#include "tls/tls_session_ticket_keys.h"

namespace ruvia::detail {

struct http_server_listener_definition final {
    struct tls_identity_type final {
        explicit tls_identity_type(std::pmr::memory_resource* resource = nullptr)
            : tls_identity_type(resolved_pmr_resource_tag{}, pmr_resource_or_default(resource)) {}

        tls_identity_type(resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
            : certificate_chain_file_(resource),
              private_key_file_(resource),
              private_key_password_(resource) {}

        std::pmr::string certificate_chain_file_;
        std::pmr::string private_key_file_;
        std::pmr::string private_key_password_;
    };

    struct tls_client_certificate_policy_type final {
        explicit tls_client_certificate_policy_type(std::pmr::memory_resource* resource = nullptr,
            tls_client_certificate_requirement configured_requirement =
                tls_client_certificate_requirement::optional)
            : tls_client_certificate_policy_type(resolved_pmr_resource_tag{}, pmr_resource_or_default(resource),
                  configured_requirement) {}

        tls_client_certificate_policy_type(resolved_pmr_resource_tag, std::pmr::memory_resource* resource,
            tls_client_certificate_requirement configured_requirement)
            : verify_file_(resource),
              requirement_(configured_requirement) {}

        std::pmr::string verify_file_;
        tls_client_certificate_requirement requirement_{tls_client_certificate_requirement::optional};
    };

    struct tls_type final {
        struct sni_identity_type final {
            explicit sni_identity_type(std::pmr::memory_resource* resource = nullptr)
                : sni_identity_type(resolved_pmr_resource_tag{}, pmr_resource_or_default(resource)) {}

            sni_identity_type(resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
                : host_(resource),
                  identity_(resolved_pmr_resource_tag{}, resource) {}

            std::pmr::string host_;
            tls_identity_type identity_;
        };

        explicit tls_type(std::pmr::memory_resource* resource = nullptr)
            : tls_type(resolved_pmr_resource_tag{}, pmr_resource_or_default(resource)) {}

        tls_type(resolved_pmr_resource_tag, std::pmr::memory_resource* resource)
            : identity_(resolved_pmr_resource_tag{}, resource),
              sni_identities_(resource),
              alt_svc_(resource) {}

        tls_identity_type identity_;
        std::optional<tls_client_certificate_policy_type> client_certificates_;
        bool http3_early_data_{};
        std::pmr::vector<sni_identity_type> sni_identities_;
        // Empty disables automatic injection. Otherwise this complete field
        // value is borrowed by every TCP/TLS request context on this listener.
        std::pmr::string alt_svc_;
        // Generated once by server configuration validation and shared
        // read-only by every worker's TCP and QUIC TLS contexts.
        std::optional<tls_session_ticket_keys> session_ticket_keys_;
    };

    struct plain_http_type final {};

    struct redirect_http_to_https_type final {
        std::uint16_t https_port_;
    };

    using transport_type = std::variant<plain_http_type, tls_type, redirect_http_to_https_type>;

    http_server_listener_definition(asio::ip::tcp::endpoint configured_endpoint,
        transport_type configured_transport = plain_http_type{},
        std::optional<http3_listen_config> configured_http3 = {})
        : endpoint_(std::move(configured_endpoint)),
          transport_(std::move(configured_transport)),
          http3_(std::move(configured_http3)) {}

    [[nodiscard]] http_server_listener_definition clone(std::pmr::memory_resource* resource) const;

    asio::ip::tcp::endpoint endpoint_;
    transport_type transport_;
    // When enabled, UDP binds this TLS listener's address and numeric port.
    std::optional<http3_listen_config> http3_;
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
