#pragma once

#include <optional>

#include <openssl/types.h>

#include "server/HttpServerListener.h"

namespace ruvia::detail {

// SSL_CTX retains the borrowed password storage; identity must outlive context.
void configureHttpServerTlsIdentity(SSL_CTX* context,
    const HttpServerListenerDefinition::TlsIdentity& identity,
    const std::optional<HttpServerListenerDefinition::TlsClientCertificatePolicy>&
        clientCertificates);

}  // namespace ruvia::detail
