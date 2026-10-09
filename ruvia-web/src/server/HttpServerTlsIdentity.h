#pragma once

#include <optional>

#include <openssl/types.h>

#include "server/HttpServerListener.h"

namespace ruvia::detail {

void validateHttpServerTlsIdentity(
    const HttpServerListenerDefinition::TlsIdentity& identity);
void validateHttpServerTlsClientCertificatePolicy(
    const HttpServerListenerDefinition::TlsClientCertificatePolicy& policy);

// Borrows identity only while loading. Restores the caller's password callback
// and userdata on both success and failure.
// An empty password can use a caller callback; default terminal prompting is disabled.
// Invalid identity or client CA configuration is rejected before loading files.
void configureHttpServerTlsIdentity(SSL_CTX* context,
    const HttpServerListenerDefinition::TlsIdentity& identity,
    const std::optional<HttpServerListenerDefinition::TlsClientCertificatePolicy>&
        clientCertificates);

}  // namespace ruvia::detail
