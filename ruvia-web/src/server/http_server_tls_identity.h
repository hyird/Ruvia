#pragma once

#include <memory>
#include <memory_resource>
#include <optional>

#include <openssl/types.h>

#include "ruvia/web/server_config.h"

#include "server/http_server_listener.h"

namespace ruvia::detail {

// Validates every TLS setting of one listener before any credential is read,
// then parses each identity (primary, SNI and local-address identities) from
// its PEM files or in-memory PEM, and the client CA bundle, exactly once.
// Invalid settings throw std::invalid_argument; unreadable, mismatched or
// invalid credentials throw asio::system_error. File names reach OpenSSL as
// UTF-8, which its file BIOs decode (on Windows into _wfopen), so no path is
// narrowed through the ANSI code page. Passwords and PEM text are only
// borrowed while their identity is parsed and are never copied; an empty
// password uses the noninteractive default and terminal prompting is disabled.
[[nodiscard]] http_server_listener_definition::tls load_http_server_tls(
    const tls_config& config, std::pmr::memory_resource* resource);

// The identities of `config` alone, validated then parsed as by
// load_http_server_tls(); for application::replace_tls_identities().
[[nodiscard]] std::shared_ptr<const http_server_tls_identities> load_http_server_tls_identities(
    const tls_config& config, std::pmr::memory_resource* resource);

// Throws std::invalid_argument unless `config` keeps the client certificate
// policy (the same CA path and requirement) and the outgoing record limit
// `current` was loaded with: replacing identities never changes them.
void ensure_same_http_server_tls_policy(const http_server_listener_definition::tls& current, const tls_config& config);

// Installs client-certificate verification into a worker's TLS context.
// Never touches the file system.
void configure_http_server_client_certificates(SSL_CTX& context_value,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy>& client_certificates);

}  // namespace ruvia::detail
