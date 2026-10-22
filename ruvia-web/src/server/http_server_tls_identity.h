#pragma once

#include <memory_resource>
#include <optional>

#include <openssl/types.h>

#include "ruvia/web/server_config.h"

#include "server/http_server_listener.h"

namespace ruvia::detail {

// Validates every TLS setting of one listener before any file is read, then
// parses each certificate/key PEM (primary and SNI identities) and the client
// CA bundle exactly once. Throws on invalid settings and on unreadable,
// mismatched or invalid credentials. File names reach OpenSSL as UTF-8, which
// its file BIOs decode (on Windows into _wfopen), so no path is narrowed
// through the ANSI code page. Passwords are only borrowed while their key is
// decrypted and are never copied; an empty password uses the noninteractive
// default and terminal prompting is disabled.
[[nodiscard]] http_server_listener_definition::tls load_http_server_tls(
    const tls_config& config, std::pmr::memory_resource* resource);

// Installs a loaded certificate (and client-certificate verification) into a
// worker's TLS context. Never touches the file system.
void configure_http_server_tls_identity(SSL_CTX* context_value, const tls_certificate& certificate,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy>&
        client_certificates);

}  // namespace ruvia::detail
