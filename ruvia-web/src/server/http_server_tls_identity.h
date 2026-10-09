#pragma once

#include <optional>

#include <openssl/types.h>

#include "server/http_server_listener.h"

namespace ruvia::detail {

void validate_http_server_tls_identity(
    const http_server_listener_definition::tls_identity_type& identity);
void validate_http_server_tls_client_certificate_policy(
    const http_server_listener_definition::tls_client_certificate_policy_type& policy);

// Borrows identity only while loading. Restores the caller's password callback
// and userdata on both success and failure.
// An empty password can use a caller callback; default terminal prompting is disabled.
// Invalid identity or client CA configuration is rejected before loading files.
void configure_http_server_tls_identity(SSL_CTX* context_value,
    const http_server_listener_definition::tls_identity_type& identity,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy_type>&
        client_certificates);

}  // namespace ruvia::detail
