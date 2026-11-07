#include "server/http_server_tls_contexts.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <stdexcept>
#include <string_view>

#include <asio/ssl/context.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#include "ruvia/http/http_ascii.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include "ruvia/http/http3_quic_connection.h"
#endif

#include "server/http_server_tls_identity.h"
#include "tls/tls_protocol_policy.h"

namespace ruvia::detail {
namespace {

int quic_resumption_context_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int generate_quic_ticket(SSL* ssl, void*) noexcept {
    const auto* context = static_cast<const std::array<unsigned char, 32>*>(
        SSL_get_ex_data(ssl, quic_resumption_context_index()));
    return context != nullptr &&
           SSL_SESSION_set1_ticket_appdata(SSL_get0_session(ssl), context->data(), context->size()) == 1;
}

SSL_TICKET_RETURN decrypt_quic_ticket(SSL* ssl, SSL_SESSION* ticket,
    const unsigned char*, std::size_t, SSL_TICKET_STATUS status, void*) noexcept {
    if (status != SSL_TICKET_SUCCESS && status != SSL_TICKET_SUCCESS_RENEW) {
        return SSL_TICKET_RETURN_IGNORE_RENEW;
    }
    const auto* context = static_cast<const std::array<unsigned char, 32>*>(
        SSL_get_ex_data(ssl, quic_resumption_context_index()));
    void* remembered = nullptr;
    std::size_t remembered_size = 0;
    // RFC 9001 §4.6.1 and RFC 9114 §7.2.8: changed transport limits or
    // SETTINGS require a fresh handshake, not acceptance of stale 0-RTT.
    if (context == nullptr || SSL_SESSION_get0_ticket_appdata(ticket, &remembered, &remembered_size) != 1 ||
        remembered_size != context->size() ||
        CRYPTO_memcmp(remembered, context->data(), context->size()) != 0) {
        return SSL_TICKET_RETURN_IGNORE_RENEW;
    }
    return status == SSL_TICKET_SUCCESS ? SSL_TICKET_RETURN_USE : SSL_TICKET_RETURN_USE_RENEW;
}

// Every HTTP version this server speaks over TLS on TCP, in preference order;
// RFC 7301 §3.2 reserves the alert for offers naming none of them.
int select_tcp_alpn_protocol(SSL*, const unsigned char** out, unsigned char* out_length,
    const unsigned char* in, unsigned int in_length, void*) noexcept {
    static constexpr unsigned char protocols[] = {
        2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1', 8, 'h', 't', 't', 'p', '/', '1', '.', '0'};
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), out_length, protocols,
            static_cast<unsigned int>(sizeof(protocols)), in, in_length) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

// RFC 9114 §3.1: an HTTP/3 connection negotiates "h3", and RFC 9001 §8.1
// requires the handshake to fail when ALPN agrees on nothing.
int select_quic_alpn_protocol(SSL*, const unsigned char** out, unsigned char* out_length,
    const unsigned char* in, unsigned int in_length, void*) noexcept {
    static constexpr unsigned char protocols[] = {2, 'h', '3'};
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), out_length, protocols,
            static_cast<unsigned int>(sizeof(protocols)), in, in_length) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

// RFC 6066 §3: a session never resumes under a server_name other than the one
// it was established with. Each connection's session ID context names its
// offered server_name (ASCII case-insensitively), and OpenSSL resumes a
// session -- TLS 1.2 by ticket or session ID, TLS 1.3 by PSK -- only when the
// contexts match, so any other name, or none, gets a full handshake (RFC 9846
// §4.6.1 permits declining such a PSK). Names longer than a context are bound
// by their SHA-256 digest.
int bind_session_to_server_name(SSL* ssl, int* alert, void*) noexcept {
    // Without server_name: one NUL byte, which no host name contains.
    std::array<unsigned char, SSL_MAX_SID_CTX_LENGTH> context{};
    std::size_t context_size = 1;
    const unsigned char* extension = nullptr;
    std::size_t extension_size = 0;
    if (SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_server_name, &extension, &extension_size) == 1) {
        // One non-empty host_name entry: any other ServerNameList shape fails
        // the handshake before OpenSSL could resume a session under it.
        if (extension_size < 5 || static_cast<std::size_t>((extension[0] << 8) | extension[1]) != extension_size - 2 ||
            extension[2] != TLSEXT_NAMETYPE_host_name) {
            *alert = SSL_AD_DECODE_ERROR;
            return SSL_CLIENT_HELLO_ERROR;
        }
        const auto name_size = static_cast<std::size_t>((extension[3] << 8) | extension[4]);
        if (name_size == 0 || name_size != extension_size - 5 || name_size > TLSEXT_MAXLEN_host_name) {
            *alert = SSL_AD_DECODE_ERROR;
            return SSL_CLIENT_HELLO_ERROR;
        }
        if (name_size <= context.size()) {
            for (std::size_t i = 0; i < name_size; ++i) {
                context[i] = http_ascii_to_lower(extension[5 + i]);
            }
            context_size = name_size;
        } else {
            std::array<unsigned char, TLSEXT_MAXLEN_host_name> name;
            for (std::size_t i = 0; i < name_size; ++i) {
                name[i] = http_ascii_to_lower(extension[5 + i]);
            }
            unsigned int digest_size = 0;
            if (EVP_Digest(name.data(), name_size, context.data(), &digest_size, EVP_sha256(), nullptr) != 1 ||
                digest_size != context.size()) {
                *alert = SSL_AD_INTERNAL_ERROR;
                return SSL_CLIENT_HELLO_ERROR;
            }
            context_size = digest_size;
        }
    }
    if (SSL_set_session_id_context(ssl, context.data(), static_cast<unsigned int>(context_size)) != 1) {
        *alert = SSL_AD_INTERNAL_ERROR;
        return SSL_CLIENT_HELLO_ERROR;
    }
    return SSL_CLIENT_HELLO_SUCCESS;
}

// RFC 6066 SNI: switch the connection to the per-host SSL_CTX of its family
// when the client's server name matches a configured certificate; otherwise
// keep the default identity. Answering an unrecognized name with the default
// certificate, rather than RFC 9325 §3.7's unrecognized_name alert, is a
// product choice documented on tls_config.
int select_sni_context(SSL* ssl, int*, void* arg) noexcept {
    if (ssl == nullptr || arg == nullptr) {
        return SSL_TLSEXT_ERR_OK;
    }
    const char* name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (name == nullptr) {
        return SSL_TLSEXT_ERR_OK;
    }
    const std::string_view server_name(name);
    const auto& selection = *static_cast<const http_server_sni_selection*>(arg);
    const auto* tls = selection.session_->tls();
    if (tls == nullptr) {
        return SSL_TLSEXT_ERR_OK;
    }
    auto& contexts = *selection.contexts_;
    const auto count = (std::min)(tls->sni_identities_.size(), contexts.size());
    for (std::size_t i = 0; i < count; ++i) {
        if (http_ascii_equals_ignore_case(tls->sni_identities_[i].host_, server_name)) {
            // A name with its own certificate must be served with it: failing
            // to switch to its context fails the handshake.
            if (SSL_set_SSL_CTX(ssl, contexts[i].native_handle()) == nullptr) {
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            }
            break;
        }
    }
    return SSL_TLSEXT_ERR_OK;
}

enum class tls_context_family : unsigned char {
    tcp,
    quic,
};

void configure_family(http_server_session_config& session, tls_context_family family) {
    const auto* tls = session.tls();
    auto& ticket_keys = *session.ticket_keys_;
    const bool quic = family == tls_context_family::quic;
    const bool early_data = quic && session.http3_->early_data_;
    auto& sni_contexts = quic ? session.quic_sni_contexts_ : session.sni_contexts_;
    auto& primary = quic ? session.quic_tls_context_ : session.tls_context_;
    auto& selection = quic ? session.quic_sni_ : session.tls_sni_;
    const bool has_sni = !tls->sni_identities_.empty();
    const auto configure = [tls, &ticket_keys, &selection, quic, early_data, has_sni](asio::ssl::context& context_value,
                               const tls_certificate& certificate) {
        auto* native = context_value.native_handle();
        if (quic) {
            // RFC 9001 §4.2: QUIC requires TLS 1.3.
            if (SSL_CTX_set_min_proto_version(native, TLS1_3_VERSION) != 1 ||
                SSL_CTX_set_max_proto_version(native, TLS1_3_VERSION) != 1) {
                throw std::runtime_error("failed to restrict the QUIC TLS context to TLS 1.3");
            }
            SSL_CTX_set_alpn_select_cb(native, select_quic_alpn_protocol, nullptr);
            if (SSL_CTX_set_max_early_data(native, early_data ? UINT32_MAX : 0) != 1 ||
                SSL_CTX_set_session_ticket_cb(native, generate_quic_ticket, decrypt_quic_ticket, nullptr) != 1) {
                throw std::runtime_error("failed to configure QUIC session tickets");
            }
            if (early_data) {
                // Ticket keys are shared across workers, not OpenSSL's local
                // replay cache. RFC 8470 admission rejects unsafe early requests.
                SSL_CTX_set_options(native, SSL_OP_NO_ANTI_REPLAY);
            }
        } else {
            context_value.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 |
                                      asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 |
                                      asio::ssl::context::no_tlsv1_1 | asio::ssl::context::single_dh_use);
            apply_tls_protocol_policy(*native, tls->max_outgoing_records_);
            SSL_CTX_set_alpn_select_cb(native, select_tcp_alpn_protocol, nullptr);
        }
        // RFC 9325 §4.2.1: the server's order decides, whatever the client prefers.
        SSL_CTX_set_options(native, SSL_OP_CIPHER_SERVER_PREFERENCE);
        configure_http_server_tls_identity(native, certificate, tls->client_certificates_);
        ticket_keys.install(*native);
        // The client hello callback runs before OpenSSL looks up the offered
        // session, which the server_name callback would be too late for.
        SSL_CTX_set_client_hello_cb(native, &bind_session_to_server_name, nullptr);
        if (has_sni) {
            SSL_CTX_set_tlsext_servername_callback(native, &select_sni_context);
            SSL_CTX_set_tlsext_servername_arg(native, &selection);
        }
    };

    // Per-host SNI certificates first, so the lookup can point at stable storage.
    sni_contexts.reserve(tls->sni_identities_.size());
    for (const auto& sni : tls->sni_identities_) {
        auto& context_value = sni_contexts.emplace_back(asio::ssl::context::tls_server);
        configure(context_value, sni.certificate_);
    }
    primary.emplace(asio::ssl::context::tls_server);
    configure(*primary, tls->certificate_);
}

}  // namespace

#if defined(RUVIA_ENABLE_HTTP3)
void configure_http3_server_tls_session(SSL& tls, const http3_quic_connection& connection,
    const http3_connection_options& options, bool early_data,
    std::array<unsigned char, 32>& resumption_context) {
    const auto* parameters = ngtcp2_conn_get_local_transport_params2(connection.native_handle());
    // Encode only remembered limits: connection IDs and reset tokens change
    // for each connection and must not prevent legitimate ticket resumption.
    const std::array<std::uint64_t, 18> values{
        ngtcp2_conn_get_client_chosen_version2(connection.native_handle()),
        parameters->initial_max_data,
        parameters->initial_max_stream_data_bidi_local,
        parameters->initial_max_stream_data_bidi_remote,
        parameters->initial_max_stream_data_uni,
        parameters->initial_max_streams_bidi,
        parameters->initial_max_streams_uni,
        parameters->active_connection_id_limit,
        parameters->max_datagram_frame_size,
        parameters->max_udp_payload_size,
        static_cast<std::uint64_t>(parameters->disable_active_migration),
        parameters->max_idle_timeout,
        options.max_field_section_size_,
        options.qpack_max_dtable_capacity_,
        options.qpack_blocked_streams_,
        static_cast<std::uint64_t>(options.enable_connect_protocol_),
        static_cast<std::uint64_t>(options.enable_h3_datagram_),
        static_cast<std::uint64_t>(early_data)};
    std::array<unsigned char, values.size() * sizeof(std::uint64_t)> encoded{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        for (std::size_t byte = 0; byte < sizeof(std::uint64_t); ++byte) {
            encoded[i * sizeof(std::uint64_t) + byte] =
                static_cast<unsigned char>(values[i] >> (8 * (sizeof(std::uint64_t) - byte - 1)));
        }
    }
    unsigned int digest_size = 0;
    const auto index = quic_resumption_context_index();
    if (index < 0 ||
        EVP_Digest(encoded.data(), encoded.size(), resumption_context.data(), &digest_size, EVP_sha256(), nullptr) != 1 ||
        digest_size != resumption_context.size() ||
        SSL_set_ex_data(&tls, index, &resumption_context) != 1 ||
        SSL_set_quic_tls_early_data_enabled(&tls, early_data ? 1 : 0) != 1) {
        throw std::runtime_error("failed to bind QUIC TLS resumption settings");
    }
}
#endif

void configure_http_server_tls_contexts(http_server_session_config& session) {
    session.tls_context_.reset();
    session.sni_contexts_.clear();
    session.quic_tls_context_.reset();
    session.quic_sni_contexts_.clear();
    session.ticket_keys_.reset();
    const auto* tls = session.tls();
    if (tls == nullptr) {
        return;
    }
    auto& ticket_keys = session.ticket_keys_.emplace();
    if (!tls_session_ticket_keys(*tls).copy_to(std::chrono::steady_clock::now(), ticket_keys)) {
        throw std::runtime_error("failed to generate TLS session ticket keys");
    }
    configure_family(session, tls_context_family::tcp);
    if (session.http3_.has_value()) {
        configure_family(session, tls_context_family::quic);
    }
}

}  // namespace ruvia::detail
