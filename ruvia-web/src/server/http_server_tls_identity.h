#pragma once

#include <optional>

#include <openssl/types.h>

#include "server/http_server_listener.h"

namespace ruvia::detail {

void validate_http_server_tls_identity(
    const http_server_listener_definition::tls_identity_type& identity);
void validate_http_server_tls_client_certificate_policy(
    const http_server_listener_definition::tls_client_certificate_policy_type& policy);

// Parses every certificate/key PEM (primary and SNI identities) and the client
// CA bundle of one listener exactly once, during server configuration
// validation. Throws on unreadable, mismatched or invalid credentials, so a
// bad identity fails validation instead of worker startup. An empty password
// uses the noninteractive default; terminal prompting is disabled.
void load_http_server_tls_credentials(http_server_listener_definition::tls_type& tls);

// Installs the identity (and client-certificate verification) that
// load_http_server_tls_credentials preloaded into a worker's TCP or QUIC
// context. Never touches the file system.
void configure_http_server_tls_identity(SSL_CTX* context_value,
    const http_server_listener_definition::tls_identity_type& identity,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy_type>&
        client_certificates);

}  // namespace ruvia::detail
