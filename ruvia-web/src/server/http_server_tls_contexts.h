#pragma once

#include <array>
#include <cstdint>

#include <asio/ip/address.hpp>
#include <openssl/types.h>

#include "server/http_server_listener.h"
#include "server/http_server_options.h"

#if defined(RUVIA_ENABLE_HTTP3)
namespace ruvia {
class http3_quic_connection;
struct http3_connection_options;
}  // namespace ruvia
#endif

namespace ruvia::detail {

// Builds one worker's TLS contexts of a listener at startup, from its loaded
// definition: the TCP context (TLS 1.1..1.3, h2 only with TLS >= 1.2,
// http/1.1, http/1.0, or connection dispatch's custom ALPN) and, with an
// HTTP/3 service, the QUIC context (TLS 1.3 only, ALPN h3 only, opt-in early
// data). Neither carries an identity: each
// handshake selects one from the identity set the session holds when its
// server_name is known (exact SNI, wildcard SNI, the local address bound by
// bind_tls_local_address(), else the primary) and installs it on its own SSL.
// Both bind resumption to the offered server_name and share the worker's copy
// of the listener's ticket keys. Replacing identities never rebuilds them, so
// their session caches and ticket keys survive it. observe_client_hello
// installs the message callback that delivers each connection's ClientHello
// to its observe_tls_client_hello() observation; without it no observation
// branch is installed. A listener without TLS keeps no context.
void configure_http_server_tls_contexts(http_server_session_config& session, bool observe_client_hello);

using prepared_http_server_session =
    std::unique_ptr<http_server_session_config>;

// Builds a stable-address candidate off the owner loop. Its contexts borrow
// only its own fields; move the owning pointer, never the session itself.
[[nodiscard]] prepared_http_server_session prepare_http_server_session_config(
    const http_server_listener_definition& definition, bool observe_client_hello);

// Attaches the local address a connection arrived at, which identity
// selection matches against local-address identities. The caller keeps it
// alive and unchanged for the SSL's whole lifetime; a connection without one
// never matches them.
void bind_tls_local_address(SSL& tls, const asio::ip::address& local_address);

// A selector needs the same stable endpoint/connection binding as an observer,
// even when no observer is registered. Does not install message observation.
[[nodiscard]] bool tls_certificate_selection_enabled(const http_server_session_config& session) noexcept;

// One connection's on_tls_client_hello delivery: every field of the record
// except the message, which the ClientHello supplies.
struct tls_client_hello_observation final {
    tls_client_hello_callback_ref callback_;
    server_observation_access::tls_client_hello_fields fields_;
};

// Attaches `observation` to a connection's SSL created from a context that
// configure_http_server_tls_contexts() built with observe_client_hello. The
// caller keeps it alive and unchanged for the SSL's whole lifetime.
void observe_tls_client_hello(SSL& tls, const tls_client_hello_observation& observation);

#if defined(RUVIA_ENABLE_HTTP3)
// Called after binding the QUIC connection, before receiving its Initial.
// `version` is the QUIC version of the client's first Initial. The caller
// owns resumption_context for the entire SSL lifetime.
void configure_http3_server_tls_session(SSL& tls, const http3_quic_connection& connection,
    std::uint32_t version, const http3_connection_options& options, bool early_data,
    std::array<unsigned char, 32>& resumption_context);
#endif
}  // namespace ruvia::detail
