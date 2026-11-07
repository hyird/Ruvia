#pragma once

#include <array>

#include "server/http_server_listener.h"

namespace ruvia {
class http3_quic_connection;
struct http3_connection_options;
}  // namespace ruvia

namespace ruvia::detail {

// Builds one worker's TLS contexts of a listener from its loaded definition:
// the TCP contexts (TLS 1.2/1.3, ALPN h2, http/1.1, http/1.0) and, when the
// listener has an HTTP/3 service, the QUIC contexts (TLS 1.3 only, ALPN h3
// only, opt-in early data). Both families serve the same identities, select
// per-host SNI certificates the same way, bind resumption to the offered
// server_name, and share the worker's copy of the listener's ticket keys.
// A listener without TLS keeps no context.
void configure_http_server_tls_contexts(http_server_session_config& session);

// Called after binding the QUIC connection, before receiving its Initial.
// The caller owns resumption_context for the entire SSL lifetime.
void configure_http3_server_tls_session(SSL& tls, const http3_quic_connection& connection,
    const http3_connection_options& options, bool early_data,
    std::array<unsigned char, 32>& resumption_context);

}  // namespace ruvia::detail
