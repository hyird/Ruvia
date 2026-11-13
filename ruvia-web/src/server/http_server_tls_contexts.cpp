
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include <asio/ip/address.hpp>
#include <asio/ssl/context.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/ssl.h>

#include "ruvia/http/http_ascii.h"

#include "server/connection_lifecycle.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include "ruvia/http/http3_quic_connection.h"
#endif

#include "auth/constant_time.h"
#include "server/http_server_options.h"
#include "tls/tls_credential.h"
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
        !constant_time_bytes_equal(
            std::string_view(static_cast<const char*>(remembered), remembered_size),
            std::string_view(reinterpret_cast<const char*>(context->data()), context->size()))) {
        return SSL_TICKET_RETURN_IGNORE_RENEW;
    }
    return status == SSL_TICKET_SUCCESS ? SSL_TICKET_RETURN_USE : SSL_TICKET_RETURN_USE_RENEW;
}

int client_hello_observation_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int local_address_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

bool select_handshake_certificate(SSL* ssl, http_server_session_config& session,
    std::string_view server_name, int* alert) noexcept;

// OpenSSL reports each inbound handshake message once it is reassembled and
// before it processes it, so a ClientHello reaches the observer before the
// client hello, server_name and certificate callbacks run.
void deliver_client_hello(int writing, int content_type, const void* bytes, std::size_t size, SSL* ssl) noexcept {
    // msg_type and the 3-byte length (RFC 9846 §4) precede every body.
    if (writing != 0 || content_type != SSL3_RT_HANDSHAKE || size < 4 ||
        *static_cast<const unsigned char*>(bytes) != SSL3_MT_CLIENT_HELLO) {
        return;
    }
    const auto* observation = static_cast<const tls_client_hello_observation*>(
        SSL_get_ex_data(ssl, client_hello_observation_index()));
    if (observation == nullptr || !observation->callback_) {
        return;
    }
    auto fields = observation->fields_;
    fields.message_ = std::span(static_cast<const std::byte*>(bytes), size);
    observation->callback_(server_observation_access::tls_client_hello(fields));
}

// TLS over TCP keeps the protocol policy's outgoing record count.
void observe_tcp_messages(int writing, int version, int content_type, const void* bytes, std::size_t size,
    SSL* ssl, void* argument) noexcept {
    count_tls_records(writing, version, content_type, bytes, size, ssl, argument);
    deliver_client_hello(writing, content_type, bytes, size, ssl);
}

// QUIC protects its own packets (RFC 9001 §4): there are no TLS records to count.
void observe_quic_messages(int writing, int, int content_type, const void* bytes, std::size_t size, SSL* ssl,
    void*) noexcept {
    deliver_client_hello(writing, content_type, bytes, size, ssl);
}

int select_tcp_alpn(SSL* ssl, std::span<const unsigned char> protocols,
    const unsigned char** out, unsigned char* out_length,
    const unsigned char* in, unsigned int in_length) noexcept {
    if (SSL_version(ssl) >= TLS1_2_VERSION) {
        return SSL_select_next_proto(const_cast<unsigned char**>(out), out_length, protocols.data(),
                   static_cast<unsigned int>(protocols.size()), in, in_length) == OPENSSL_NPN_NEGOTIATED
                   ? SSL_TLSEXT_ERR_OK
                   : SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    // RFC 9113 §9.2: h2 cannot be selected on TLS 1.1, even when a raw
    // connection listener explicitly lists it among its own protocols.
    for (std::size_t offset = 0; offset < protocols.size();) {
        const auto* protocol = protocols.data() + offset;
        const auto length = protocol[0];
        offset += static_cast<std::size_t>(length) + 1;
        if (length == 2 && protocol[1] == 'h' && protocol[2] == '2') {
            continue;
        }
        if (SSL_select_next_proto(const_cast<unsigned char**>(out), out_length, protocol,
                static_cast<unsigned int>(length) + 1, in, in_length) == OPENSSL_NPN_NEGOTIATED) {
            return SSL_TLSEXT_ERR_OK;
        }
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

// Every HTTP version this server speaks over TLS on TCP, in preference order;
// RFC 7301 §3.2 reserves the alert for offers naming none of them.
int select_tcp_alpn_protocol(SSL* ssl, const unsigned char** out, unsigned char* out_length,
    const unsigned char* in, unsigned int in_length, void*) noexcept {
    static constexpr unsigned char protocols[] = {
        2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1', 8, 'h', 't', 't', 'p', '/', '1', '.', '0'};
    return select_tcp_alpn(ssl, protocols, out, out_length, in, in_length);
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

// A connection-dispatch listener offers its own protocols (tls_config::alpn_),
// still in server preference and failing an offer that names none of them.
int select_connection_alpn_protocol(SSL* ssl, const unsigned char** out, unsigned char* out_length,
    const unsigned char* in, unsigned int in_length, void* argument) noexcept {
    const auto& protocols = *static_cast<const std::string*>(argument);
    return select_tcp_alpn(ssl,
        {reinterpret_cast<const unsigned char*>(protocols.data()), protocols.size()},
        out, out_length, in, in_length);
}

// RFC 6066 §3: a session never resumes under a server_name other than the one
// it was established with. Each connection's session ID context names its
// offered server_name (ASCII case-insensitively), and OpenSSL resumes a
// session -- TLS 1.2 by ticket or session ID, TLS 1.3 by PSK -- only when the
// contexts match, so any other name, or none, gets a full handshake (RFC 9846
// §4.6.1 permits declining such a PSK). Names longer than a context are bound
// by their SHA-256 digest.
int bind_session_to_server_name(SSL* ssl, int* alert, void* argument) noexcept {
    // Without server_name: one NUL byte, which no host name contains.
    std::array<unsigned char, SSL_MAX_SID_CTX_LENGTH> context{};
    std::size_t context_size = 1;
    const unsigned char* extension = nullptr;
    std::size_t extension_size = 0;
    std::string_view server_name;
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
        server_name = std::string_view(reinterpret_cast<const char*>(extension + 5), name_size);
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
    if (!select_handshake_certificate(ssl, *static_cast<http_server_session_config*>(argument), server_name, alert)) {
        return SSL_CLIENT_HELLO_ERROR;
    }
    return SSL_CLIENT_HELLO_SUCCESS;
}

std::span<const std::byte> client_hello_list(SSL* ssl, unsigned int type, std::size_t prefix) {
    const unsigned char* bytes = nullptr;
    std::size_t size = 0;
    if (SSL_client_hello_get0_ext(ssl, type, &bytes, &size) != 1) {
        return {};
    }
    if (size < prefix) {
        throw std::invalid_argument("malformed TLS selection extension");
    }
    const auto length = prefix == 1 ? static_cast<std::size_t>(bytes[0])
                                    : static_cast<std::size_t>((bytes[0] << 8) | bytes[1]);
    if (length != size - prefix) {
        throw std::invalid_argument("malformed TLS selection extension length");
    }
    return {reinterpret_cast<const std::byte*>(bytes + prefix), length};
}

std::vector<std::uint16_t> decode_u16_list(
    std::span<const std::byte> raw) {
    if (raw.size() % 2U != 0U) {
        throw std::invalid_argument("malformed TLS uint16 extension list");
    }
    std::vector<std::uint16_t> decoded;
    decoded.reserve(raw.size() / 2U);
    for (std::size_t i = 0; i < raw.size(); i += 2U) {
        const auto hi = static_cast<std::uint16_t>(std::to_integer<unsigned int>(raw[i]));
        const auto lo = static_cast<std::uint16_t>(std::to_integer<unsigned int>(raw[i + 1U]));
        decoded.push_back(static_cast<std::uint16_t>((hi << 8U) | lo));
    }
    return decoded;
}

std::vector<std::string_view> decode_alpn_list(
    std::span<const std::byte> raw) {
    std::vector<std::string_view> protocols;
    for (std::size_t pos = 0; pos < raw.size();) {
        const auto length = static_cast<std::size_t>(std::to_integer<unsigned int>(raw[pos]));
        if (length == 0U || pos + 1U + length > raw.size()) {
            throw std::invalid_argument("malformed TLS ALPN extension list");
        }
        protocols.emplace_back(
            reinterpret_cast<const char*>(raw.data() + pos + 1U), length);
        pos += 1U + length;
    }
    return protocols;
}

// ClientHello precedes OpenSSL's status_request parsing. Give this SSL its
// own requested staple so replacement cannot invalidate its selected pair.
bool present_fixed_identity(SSL& ssl, const http_server_tls_identities::identity& identity) noexcept {
    if (!identity.certificate_.present(ssl)) {
        return false;
    }
    const unsigned char* extension = nullptr;
    std::size_t size = 0;
    if (identity.ocsp_response_.empty() ||
        SSL_client_hello_get0_ext(&ssl, TLSEXT_TYPE_status_request, &extension, &size) != 1 ||
        size == 0 || extension[0] != TLSEXT_STATUSTYPE_ocsp) {
        return true;
    }
    auto* response = static_cast<unsigned char*>(OPENSSL_memdup(
        identity.ocsp_response_.data(), identity.ocsp_response_.size()));
    if (response == nullptr) {
        return false;
    }
    if (SSL_set_tlsext_status_ocsp_resp(&ssl, response, static_cast<int>(identity.ocsp_response_.size())) != 1) {
        OPENSSL_free(response);
        return false;
    }
    return true;
}

int staple_ocsp_response(SSL* ssl, void*) noexcept {
    const unsigned char* response = nullptr;
    return SSL_get_tlsext_status_ocsp_resp(ssl, &response) > 0
               ? SSL_TLSEXT_ERR_OK
               : SSL_TLSEXT_ERR_NOACK;
}

// Runs before session lookup, so neither resumption nor missing SNI bypasses a
// selector rejection. A HelloRetryRequest retains the first selected identity.
bool select_handshake_certificate(SSL* ssl, http_server_session_config& session,
    std::string_view server_name, int* alert) noexcept {
    if (SSL_get_certificate(ssl) != nullptr) {
        return true;
    }
    const auto& identities = *session.tls()->identities_;
    const auto* local = static_cast<const asio::ip::address*>(SSL_get_ex_data(ssl, local_address_index()));
    try {
        if (!identities.certificate_store_) {
            if (present_fixed_identity(*ssl, identities.select(server_name, local))) {
                return true;
            }
        } else {
            // Capture before calling user code: even a concurrent replacement
            // or a mutation made inside the callback cannot change this choice.
            const auto snapshot = tls_certificate_store_access::snapshot(*identities.certificate_store_);
            std::optional<tls_certificate_id> selected = identities.default_certificate_;
            if (identities.select_certificate_) {
                const auto* binding = static_cast<const tls_client_hello_observation*>(
                    SSL_get_ex_data(ssl, client_hello_observation_index()));
                if (!binding || binding->fields_.connection_id_ == 0 || local == nullptr) {
                    throw std::logic_error("TLS certificate selector requires connection metadata");
                }
                const unsigned char* ciphers = nullptr;
                const auto cipher_size = SSL_client_hello_get0_ciphers(ssl, &ciphers);
                const auto cipher_suites = decode_u16_list(
                    {reinterpret_cast<const std::byte*>(ciphers), cipher_size});
                const auto signature_algorithms = decode_u16_list(
                    client_hello_list(ssl, TLSEXT_TYPE_signature_algorithms, 2));
                const auto supported_versions = decode_u16_list(
                    client_hello_list(ssl, TLSEXT_TYPE_supported_versions, 1));
                const auto supported_groups = decode_u16_list(
                    client_hello_list(ssl, TLSEXT_TYPE_supported_groups, 2));
                const auto alpn = decode_alpn_list(
                    client_hello_list(ssl, TLSEXT_TYPE_application_layer_protocol_negotiation, 2));
                const tls_certificate_selection input{
                    .connection_id_ = binding->fields_.connection_id_,
                    .remote_address_ = ip_address::parse(binding->fields_.remote_address_).value_or(ip_address{}),
                    .remote_port_ = binding->fields_.remote_port_,
                    .server_name_ = server_name,
                    .local_address_ = local ? std::optional(to_ip_address(*local)) : std::nullopt,
                    .local_port_ = binding->fields_.local_port_,
                    .quic_ = binding->fields_.quic_,
                    .legacy_version_ = static_cast<std::uint16_t>(SSL_client_hello_get0_legacy_version(ssl)),
                    .cipher_suites_ = cipher_suites,
                    .signature_algorithms_ = signature_algorithms,
                    .supported_versions_ = supported_versions,
                    .supported_groups_ = supported_groups,
                    .alpn_ = alpn};
                selected = (*identities.select_certificate_)(input);
            }
            if (selected) {
                if (const auto* cert = snapshot->find(*selected); cert != nullptr && cert->present(*ssl)) {
                    return true;
                }
            }
        }
    } catch (...) {
        *alert = SSL_AD_INTERNAL_ERROR;
        return false;
    }
    *alert = SSL_AD_ACCESS_DENIED;
    return false;
}

int acknowledge_server_name(SSL*, int*, void*) noexcept {
    return SSL_TLSEXT_ERR_OK;
}

enum class tls_context_family : unsigned char {
    tcp,
    quic,
};

void configure_family(http_server_session_config& session, tls_context_family family, bool observe_client_hello) {
    const auto* tls = session.tls();
    const bool quic = family == tls_context_family::quic;
    const bool early_data = quic && session.http3_->early_data_;
    auto& context_value = (quic ? session.quic_tls_context_ : session.tls_context_).emplace(asio::ssl::context::tls_server);
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
                                  asio::ssl::context::single_dh_use);
        apply_tls_protocol_policy(*native, tls->max_outgoing_records_);
        if (session.connection_.has_value()) {
            SSL_CTX_set_alpn_select_cb(native, select_connection_alpn_protocol, &session.connection_->alpn_);
        } else {
            SSL_CTX_set_alpn_select_cb(native, select_tcp_alpn_protocol, nullptr);
        }
    }
    if (observe_client_hello) {
        SSL_CTX_set_msg_callback(native, quic ? observe_quic_messages : observe_tcp_messages);
    }
    // RFC 9325 §4.2.1: the server's order decides, whatever the client prefers.
    SSL_CTX_set_options(native, SSL_OP_CIPHER_SERVER_PREFERENCE);
    configure_http_server_client_certificates(*native, tls->client_certificates_);
    session.ticket_keys_->install(*native);
    // The client hello callback runs before OpenSSL looks up the offered
    // session, which the server_name callback would be too late for.
    SSL_CTX_set_client_hello_cb(native, &bind_session_to_server_name, &session);
    // The session outlives every SSL of its contexts, which its worker joins
    // before retiring them.
    SSL_CTX_set_tlsext_servername_callback(native, &acknowledge_server_name);
    SSL_CTX_set_tlsext_servername_arg(native, &session);
    SSL_CTX_set_tlsext_status_cb(native, &staple_ocsp_response);
}

}  // namespace

#if defined(RUVIA_ENABLE_HTTP3)
void configure_http3_server_tls_session(SSL& tls, const http3_quic_connection& connection,
    std::uint32_t version, const http3_connection_options& options, bool early_data,
    std::array<unsigned char, 32>& resumption_context) {
    // Only remembered limits (RFC 9000 §7.4.1): connection IDs and reset
    // tokens change for each connection and must not prevent legitimate
    // ticket resumption.
    std::array<std::byte, 256> parameters{};
    const auto parameter_bytes = connection.encode_early_transport_parameters(parameters);
    if (!parameter_bytes.has_value()) {
        throw std::runtime_error("failed to encode QUIC early transport parameters");
    }
    const std::array<std::uint64_t, 7> values{version, options.max_field_section_size_,
        options.qpack_max_dtable_capacity_, options.qpack_blocked_streams_,
        static_cast<std::uint64_t>(options.enable_connect_protocol_),
        static_cast<std::uint64_t>(options.enable_h3_datagram_), static_cast<std::uint64_t>(early_data)};
    std::array<unsigned char, values.size() * sizeof(std::uint64_t) + parameters.size()> encoded{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        for (std::size_t byte = 0; byte < sizeof(std::uint64_t); ++byte) {
            encoded[i * sizeof(std::uint64_t) + byte] =
                static_cast<unsigned char>(values[i] >> (8 * (sizeof(std::uint64_t) - byte - 1)));
        }
    }
    const auto encoded_size = values.size() * sizeof(std::uint64_t) + *parameter_bytes;
    std::memcpy(encoded.data() + values.size() * sizeof(std::uint64_t), parameters.data(), *parameter_bytes);
    unsigned int digest_size = 0;
    const auto index = quic_resumption_context_index();
    if (index < 0 ||
        EVP_Digest(encoded.data(), encoded_size, resumption_context.data(), &digest_size, EVP_sha256(), nullptr) != 1 ||
        digest_size != resumption_context.size() ||
        SSL_set_ex_data(&tls, index, &resumption_context) != 1 ||
        SSL_set_quic_tls_early_data_enabled(&tls, early_data ? 1 : 0) != 1) {
        throw std::runtime_error("failed to bind QUIC TLS resumption settings");
    }
}
#endif

void configure_http_server_tls_contexts(http_server_session_config& session, bool observe_client_hello) {
    session.tls_context_.reset();
    session.quic_tls_context_.reset();
    session.ticket_keys_.reset();
    const auto* tls = session.tls();
    if (tls == nullptr) {
        return;
    }
    auto& ticket_keys = session.ticket_keys_.emplace();
    if (!tls_session_ticket_keys(*tls).copy_to(std::chrono::steady_clock::now(), ticket_keys)) {
        throw std::runtime_error("failed to generate TLS session ticket keys");
    }
    configure_family(session, tls_context_family::tcp, observe_client_hello);
    if (session.http3_.has_value()) {
        configure_family(session, tls_context_family::quic, observe_client_hello);
    }
}

prepared_http_server_session prepare_http_server_session_config(
    const http_server_listener_definition& definition, bool observe_client_hello) {
    auto session = std::make_unique<http_server_session_config>(definition);
    configure_http_server_tls_contexts(*session, observe_client_hello);
    return session;
}

void bind_tls_local_address(SSL& tls, const asio::ip::address& local_address) {
    const auto index = local_address_index();
    // OpenSSL's ex_data stores a mutable pointer; selection only reads it.
    if (index < 0 || SSL_set_ex_data(&tls, index, const_cast<asio::ip::address*>(&local_address)) != 1) {
        throw std::runtime_error("failed to attach the TLS connection's local address");
    }
}

bool tls_certificate_selection_enabled(const http_server_session_config& session) noexcept {
    const auto* tls = session.tls();
    return tls != nullptr && static_cast<bool>(tls->identities_->select_certificate_);
}

void observe_tls_client_hello(SSL& tls, const tls_client_hello_observation& observation) {
    const auto index = client_hello_observation_index();
    // OpenSSL's ex_data stores a mutable pointer; the observer only reads it.
    if (index < 0 || SSL_set_ex_data(&tls, index, const_cast<tls_client_hello_observation*>(&observation)) != 1) {
        throw std::runtime_error("failed to attach the TLS ClientHello observer");
    }
}

}  // namespace ruvia::detail
