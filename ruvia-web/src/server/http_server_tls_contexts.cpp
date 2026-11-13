
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

#include "ruvia/http/http_field_syntax.h"

#include "server/connection_lifecycle.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include "ruvia/http/http3_quic_connection.h"
#endif

#include <algorithm>
#include <bit>
#include <cerrno>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <system_error>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <asio/bind_executor.hpp>
#include <asio/error.hpp>
#include <asio/post.hpp>
#include <asio/ssl/verify_mode.hpp>
#include <openssl/err.h>
#include <openssl/ocsp.h>

#include "ruvia/core/async.h"
#include "ruvia/core/blocking_pool.h"
#include "ruvia/core/failure_report.h"
#include "ruvia/core/socket.h"
#include "ruvia/core/timer.h"

#include "http/inline_http_header_views.h"
#include "http/tls_protocol_policy.h"
#include "server/app_state.h"
#include "server/http_server_options.h"
#include "server/route_table.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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

namespace ruvia::detail {
namespace {

// In-memory PEM text of a tls_identity_config, borrowed while it is parsed.
struct tls_identity_pem final {
    std::string_view certificate_chain_;
    std::string_view private_key_;
};

void validate_ocsp_response(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return;
    }
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("TLS OCSP response is too large");
    }
    ERR_clear_error();
    const auto* cursor = reinterpret_cast<const unsigned char*>(bytes.data());
    const auto* const end = cursor + bytes.size();
    const std::unique_ptr<OCSP_RESPONSE, decltype(&OCSP_RESPONSE_free)> response(
        d2i_OCSP_RESPONSE(nullptr, &cursor, static_cast<long>(bytes.size())), OCSP_RESPONSE_free);
    if (!response) {
        const auto error = ERR_peek_last_error();
        ERR_clear_error();
        if (ERR_GET_REASON(error) == ERR_R_MALLOC_FAILURE) {
            throw std::bad_alloc();
        }
        throw std::invalid_argument("TLS OCSP response must be a DER OCSPResponse");
    }
    const auto size = i2d_OCSP_RESPONSE(response.get(), nullptr);
    if (cursor != end || size <= 0 || static_cast<std::size_t>(size) != bytes.size()) {
        throw std::invalid_argument("TLS OCSP response must be a DER OCSPResponse");
    }
    // d2i also accepts BER; require the canonical DER encoding we advertise.
    std::vector<std::byte> canonical(bytes.size());
    auto* output = reinterpret_cast<unsigned char*>(canonical.data());
    if (i2d_OCSP_RESPONSE(response.get(), &output) != size ||
        !std::equal(bytes.begin(), bytes.end(), canonical.begin())) {
        throw std::invalid_argument("TLS OCSP response must be a DER OCSPResponse");
    }
}

// One identity's validated source and the password its key needs.
struct tls_identity_source final {
    std::variant<tls_identity_files, tls_identity_pem> location_;
    std::string_view private_key_password_;
    std::span<const std::byte> ocsp_response_;
};

[[nodiscard]] tls_identity_source make_tls_identity_source(
    const tls_identity_config& identity,
    std::span<const std::byte> ocsp_response = {}) {
    validate_ocsp_response(ocsp_response);
    const bool files = !identity.certificate_chain_file_.empty() || !identity.private_key_file_.empty();
    const bool pem = !identity.certificate_chain_pem_.empty() || !identity.private_key_pem_.empty();
    if (files && pem) {
        throw std::invalid_argument("TLS identity must come from PEM files or in-memory PEM, not both");
    }
    if (!pem) {
        // Both names are required: an empty one throws.
        return {.location_ = make_tls_identity_files(identity),
            .private_key_password_ = identity.private_key_password_,
            .ocsp_response_ = ocsp_response};
    }
    if (identity.certificate_chain_pem_.empty() || identity.private_key_pem_.empty()) {
        throw std::invalid_argument("in-memory TLS identity requires both certificate chain and private key PEM");
    }
    return {.location_ = tls_identity_pem{.certificate_chain_ = identity.certificate_chain_pem_,
                .private_key_ = identity.private_key_pem_},
        .private_key_password_ = identity.private_key_password_,
        .ocsp_response_ = ocsp_response};
}

[[nodiscard]] tls_certificate load_tls_identity(const tls_identity_source& source) {
    if (const auto* files = std::get_if<tls_identity_files>(&source.location_)) {
        return load_tls_certificate(*files, source.private_key_password_);
    }
    const auto& pem = std::get<tls_identity_pem>(source.location_);
    return tls_certificate::load_pem(pem.certificate_chain_, pem.private_key_, source.private_key_password_);
}

[[nodiscard]] http_server_tls_identities::identity load_fixed_tls_identity(
    const tls_identity_source& source) {
    return {load_tls_identity(source), source.ocsp_response_};
}

// RFC 6066 §3 HostName: an ASCII DNS name, without the trailing dot.
[[nodiscard]] bool is_valid_sni_host(std::string_view host) noexcept {
    return !host.empty() && !host.ends_with('.') && detail::is_valid_dns_host(host);
}

// A connection reaching an IPv4 address through a dual-stack socket reports
// its IPv4-mapped IPv6 form; both name the same local address.
[[nodiscard]] asio::ip::address canonical_local_address(const asio::ip::address& address) noexcept {
    if (address.is_v6()) {
        if (const auto v6 = address.to_v6(); v6.is_v4_mapped()) {
            return asio::ip::make_address_v4(asio::ip::v4_mapped, v6);
        }
    }
    return address;
}

[[nodiscard]] asio::ip::address make_tls_local_address(std::string_view text) {
    if (text.empty()) {
        throw std::invalid_argument("TLS local address must not be empty");
    }
    const auto parsed = ruvia::parse_ip_address(text);
    if (!parsed) {
        throw std::invalid_argument("TLS local address must be a numeric IP address");
    }
    const auto address = canonical_local_address(*parsed);
    // No connection arrives at an unspecified address.
    if (address.is_unspecified()) {
        throw std::invalid_argument("TLS local address must not be unspecified");
    }
    return address;
}

// Every identity setting of one tls_config, validated before anything is read.
class tls_identity_plan final {
public:
    tls_identity_plan(const tls_config& config);

    [[nodiscard]] std::shared_ptr<const http_server_tls_identities> load() const;

private:
    struct named_source final {
        // ASCII lowercase; a wildcard keeps the parent domain after its "*.".
        std::string name_;
        bool wildcard_;
        tls_identity_source source_;
    };

    struct addressed_source final {
        asio::ip::address address_;
        tls_identity_source source_;
    };

    std::optional<tls_identity_source> primary_;
    std::shared_ptr<tls_certificate_store> store_;
    std::shared_ptr<const tls_certificate_selector> selector_;
    tls_certificate_id default_certificate_{};
    std::vector<named_source> names_;
    std::vector<addressed_source> addresses_;
};

tls_identity_plan::tls_identity_plan(const tls_config& config) {
    if (config.certificate_store_) {
        const auto& identity = config.identity_;
        if (!identity.certificate_chain_file_.empty() || !identity.private_key_file_.empty() ||
            !identity.certificate_chain_pem_.empty() || !identity.private_key_pem_.empty() ||
            !identity.private_key_password_.empty() || !config.ocsp_response_.empty() ||
            !config.sni_.empty() || !config.local_addresses_.empty()) {
            throw std::invalid_argument("TLS certificate library cannot be combined with fixed identities");
        }
        if (!config.select_certificate_ && config.default_certificate_ == 0) {
            throw std::invalid_argument("TLS certificate library requires a selector or default certificate ID");
        }
        store_ = config.certificate_store_;
        selector_ = config.select_certificate_;
        default_certificate_ = config.default_certificate_;
        return;
    }
    if (config.select_certificate_ || config.default_certificate_ != 0) {
        throw std::invalid_argument("TLS certificate selection requires a certificate library");
    }
    primary_.emplace(make_tls_identity_source(config.identity_, config.ocsp_response_));
    names_.reserve(config.sni_.size());
    for (const auto& sni : config.sni_) {
        if (sni.host_.empty()) {
            throw std::invalid_argument("SNI host must not be empty");
        }
        // RFC 6125 §6.4.3: a wildcard is the whole left-most label.
        const bool wildcard = sni.host_.starts_with("*.");
        const auto host = std::string_view(sni.host_).substr(wildcard ? 2 : 0);
        if (!is_valid_sni_host(host)) {
            throw std::invalid_argument("SNI host is invalid");
        }
        std::string name(host);
        for (auto& byte : name) {
            byte = static_cast<char>(http_ascii_to_lower(static_cast<unsigned char>(byte)));
        }
        for (const auto& existing : names_) {
            if (existing.wildcard_ == wildcard && existing.name_ == name) {
                throw std::invalid_argument("SNI hosts must be unique");
            }
        }
        names_.push_back(named_source{
            .name_ = std::move(name),
            .wildcard_ = wildcard,
            .source_ = make_tls_identity_source(sni.identity_, sni.ocsp_response_),
        });
    }
    addresses_.reserve(config.local_addresses_.size());
    for (const auto& local : config.local_addresses_) {
        const auto address = make_tls_local_address(local.address_);
        for (const auto& existing : addresses_) {
            if (existing.address_ == address) {
                throw std::invalid_argument("TLS local addresses must be unique");
            }
        }
        addresses_.push_back(addressed_source{
            .address_ = address,
            .source_ = make_tls_identity_source(local.identity_, local.ocsp_response_),
        });
    }
}

std::shared_ptr<const http_server_tls_identities> tls_identity_plan::load() const {
    if (store_) {
        auto identities = std::make_shared<http_server_tls_identities>();
        identities->certificate_store_ = store_;
        identities->select_certificate_ = selector_;
        identities->default_certificate_ = default_certificate_;
        return identities;
    }
    auto identities = std::make_shared<http_server_tls_identities>(load_fixed_tls_identity(*primary_));
    for (const auto& named : names_) {
        auto& target = named.wildcard_ ? identities->wildcard_names_ : identities->exact_names_;
        target.emplace_back(named.name_, load_fixed_tls_identity(named.source_));
    }
    identities->local_addresses_.reserve(addresses_.size());
    for (const auto& addressed : addresses_) {
        identities->local_addresses_.push_back(http_server_tls_identities::addressed_identity{
            .address_ = addressed.address_,
            .value_ = load_fixed_tls_identity(addressed.source_),
        });
    }
    return identities;
}

[[nodiscard]] asio::ssl::verify_mode http_server_tls_verify_mode(
    tls_client_certificate_requirement requirement) noexcept {
    auto mode = asio::ssl::verify_peer;
    if (requirement == tls_client_certificate_requirement::required) {
        mode |= asio::ssl::verify_fail_if_no_peer_cert;
    }
    return mode;
}

}  // namespace

tls_certificate load_server_tls_certificate(const tls_identity_config& identity) {
    return load_tls_identity(make_tls_identity_source(identity));
}

const http_server_tls_identities::identity& http_server_tls_identities::select(
    std::string_view server_name, const asio::ip::address* local_address) const noexcept {
    if (!server_name.empty()) {
        for (const auto& named : exact_names_) {
            if (http_ascii_equals_ignore_case(named.name_, server_name)) {
                return named.value_;
            }
        }
        // RFC 6125 §6.4.3: "*" stands for exactly one non-empty label.
        if (const auto dot = server_name.find('.'); dot != std::string_view::npos && dot != 0) {
            const auto parent = server_name.substr(dot + 1);
            for (const auto& named : wildcard_names_) {
                if (http_ascii_equals_ignore_case(named.name_, parent)) {
                    return named.value_;
                }
            }
        }
    }
    if (local_address != nullptr && !local_addresses_.empty()) {
        const auto address = canonical_local_address(*local_address);
        for (const auto& addressed : local_addresses_) {
            if (addressed.address_ == address) {
                return addressed.value_;
            }
        }
    }
    return *primary_;
}

http_server_listener_definition::tls load_http_server_tls(
    const tls_config& config) {
    validate_tls_record_limit(config.max_outgoing_records_);
    // Every setting is validated before any credential is read.
    const tls_identity_plan identities(config);
    const auto& client = config.client_certificates_;
    std::optional<tls_file_name> client_ca;
    if (client.ca_file_.has_value() || client.requirement_ != tls_client_certificate_requirement::optional) {
        switch (client.requirement_) {
            case tls_client_certificate_requirement::optional:
            case tls_client_certificate_requirement::required:
                break;
            default:
                throw std::invalid_argument("TLS client certificate requirement is invalid");
        }
        if (!client.ca_file_.has_value()) {
            throw std::invalid_argument("TLS client certificates require a CA bundle");
        }
        client_ca.emplace(make_tls_file_name(*client.ca_file_));
    }

    http_server_listener_definition::tls tls(identities.load());
    tls.max_outgoing_records_ = config.max_outgoing_records_;
    if (client_ca.has_value()) {
        auto trust_store = load_tls_trust_store(*client_ca);
        tls.client_certificates_.emplace(http_server_listener_definition::tls_client_certificate_policy{
            .requirement_ = client.requirement_,
            .client_ca_ = std::move(trust_store),
            .ca_file_ = std::move(*client_ca),
        });
    }
    return tls;
}

std::shared_ptr<const http_server_tls_identities> load_http_server_tls_identities(
    const tls_config& config) {
    return tls_identity_plan(config).load();
}

void ensure_same_http_server_tls_policy(const http_server_listener_definition::tls& current, const tls_config& config) {
    if (config.max_outgoing_records_ != current.max_outgoing_records_) {
        throw std::invalid_argument("replacing TLS identities must keep the listener's outgoing record limit");
    }
    const auto& client = config.client_certificates_;
    const auto& policy = current.client_certificates_;
    const bool same = policy.has_value()
                          ? client.requirement_ == policy->requirement_ && client.ca_file_.has_value() &&
                                make_tls_file_name(*client.ca_file_) == policy->ca_file_
                          : client.requirement_ == tls_client_certificate_requirement::optional &&
                                !client.ca_file_.has_value();
    if (!same) {
        throw std::invalid_argument("replacing TLS identities must keep the listener's client certificate policy");
    }
}

void configure_http_server_client_certificates(SSL_CTX& context_value,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy>& client_certificates) {
    if (!client_certificates.has_value()) {
        return;
    }
    client_certificates->client_ca_.install(context_value);
    // A server that verifies peers without a session ID context turns every
    // resumption attempt into a fatal handshake error (SSL_CTX_set_session_id_context(3)).
    static constexpr unsigned char session_id_context[] = {'r', 'u', 'v', 'i', 'a'};
    ::ERR_clear_error();
    if (::SSL_CTX_set_session_id_context(&context_value, session_id_context, sizeof(session_id_context)) != 1) {
        throw_tls_credential_error("set_session_id_context");
    }
    SSL_CTX_set_verify(&context_value,
        static_cast<int>(http_server_tls_verify_mode(client_certificates->requirement_)), nullptr);
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

// The identity set is immutable and shared, not copied.
http_server_listener_definition::tls clone_tls(
    const http_server_listener_definition::tls& source_value) {
    http_server_listener_definition::tls result_value(source_value.identities_);
    if (const auto& policy = source_value.client_certificates_; policy.has_value()) {
        result_value.client_certificates_.emplace(http_server_listener_definition::tls_client_certificate_policy{
            .requirement_ = policy->requirement_,
            .client_ca_ = policy->client_ca_,
            .ca_file_ = tls_file_name(policy->ca_file_),
        });
    }
    result_value.max_outgoing_records_ = source_value.max_outgoing_records_;
    result_value.session_ticket_keys_ = source_value.session_ticket_keys_;
    result_value.tcp_mode_ = source_value.tcp_mode_;
    result_value.plaintext_redirect_ = source_value.plaintext_redirect_;
    return result_value;
}

http_server_listener_definition::transport_type clone_transport(
    const http_server_listener_definition::transport_type& source_value) {
    return std::visit(
        []<typename transport_type>(
            const transport_type& transport) -> http_server_listener_definition::transport_type {
            if constexpr (std::is_same_v<transport_type, http_server_listener_definition::tls>) {
                return clone_tls(transport);
            } else {
                return transport;
            }
        },
        source_value);
}

std::optional<http_server_listener_definition::connection_dispatch> clone_connection_dispatch(
    const std::optional<http_server_listener_definition::connection_dispatch>& source_value) {
    std::optional<http_server_listener_definition::connection_dispatch> result_value;
    if (source_value.has_value()) {
        result_value.emplace(source_value->alpn_);
    }
    return result_value;
}

}  // namespace

http_server_listener_definition http_server_listener_definition::clone() const {
    http_server_listener_definition result_value(
        endpoint_, clone_transport(transport_), http3_);
    result_value.connection_ = clone_connection_dispatch(connection_);
    result_value.inherited_sockets_ = inherited_sockets_;
    result_value.ipv6_only_ = ipv6_only_;
    result_value.proxy_protocol_ = proxy_protocol_;
    result_value.request_stream_ = request_stream_;
    return result_value;
}

http_server_session_config::http_server_session_config(
    const http_server_listener_definition& definition)
    : transport_(clone_transport(definition.transport_)),
      ipv6_only_(definition.ipv6_only_),
      proxy_protocol_(definition.proxy_protocol_),
      http3_(definition.http3_),
      connection_(clone_connection_dispatch(definition.connection_)),
      request_stream_(definition.request_stream_) {
    if (http3_.has_value()) {
        // RFC 7838 §3: alt-authority with an empty host names this origin's
        // host on the UDP port; "ma" bounds how long clients cache it.
        alternative_service_.append("h3=\":");
        alternative_service_.append(std::to_string(http3_->port_));
        alternative_service_.append("\"; ma=");
        alternative_service_.append(std::to_string(http3_->alt_svc_max_age_.count()));
    }
}

void http_server_session_config::adopt_tls_identities(
    std::shared_ptr<const http_server_tls_identities> identities) noexcept {
    if (auto* tls = std::get_if<http_server_listener_definition::tls>(&transport_)) {
        tls->identities_.swap(identities);
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

#if defined(_WIN32)
using socket_length = int;

[[nodiscard]] SOCKET os_socket(native_socket socket) noexcept {
    return static_cast<SOCKET>(socket);
}
#else
using socket_length = socklen_t;

[[nodiscard]] int os_socket(native_socket socket) noexcept {
    return socket;
}
#endif

// The integer SOL_SOCKET option `name`; nullopt when `socket` is no socket.
[[nodiscard]] std::optional<int> socket_option(native_socket socket, int name) noexcept {
    int value = 0;
    auto length = static_cast<socket_length>(sizeof(value));
    if (::getsockopt(os_socket(socket), SOL_SOCKET, name, reinterpret_cast<char*>(&value), &length) != 0) {
        return std::nullopt;
    }
    return value;
}

// Whether `socket` is bound to exactly `endpoint`, in its address family.
template <typename endpoint_type>
[[nodiscard]] bool bound_to(native_socket socket, const endpoint_type& endpoint) {
    endpoint_type local;
    auto length = static_cast<socket_length>(local.capacity());
    if (::getsockname(os_socket(socket), local.data(), &length) != 0 || length <= 0 ||
        static_cast<std::size_t>(length) > local.capacity() || local.data()->sa_family != endpoint.data()->sa_family) {
        return false;
    }
    local.resize(static_cast<std::size_t>(length));
    return local == endpoint;
}

// Inherited handles leave with an exec (or a Windows child process) only when
// the application hands them out again.
void mark_close_on_exec(native_socket socket) {
#if defined(_WIN32)
    if (::SetHandleInformation(reinterpret_cast<HANDLE>(os_socket(socket)), HANDLE_FLAG_INHERIT, 0) == 0) {
        throw std::system_error(static_cast<int>(::GetLastError()), asio::system_category(),
            "failed to make an inherited listener socket non-inheritable");
    }
#else
    const int flags = ::fcntl(socket, F_GETFD);
    if (flags == -1 || ::fcntl(socket, F_SETFD, flags | FD_CLOEXEC) == -1) {
        throw std::system_error(errno, asio::system_category(), "failed to mark an inherited listener socket close-on-exec");
    }
#endif
}

void ensure_socket_kind(native_socket socket, int type, [[maybe_unused]] int protocol, const char* message) {
    if (socket_option(socket, SO_TYPE) != type) {
        throw std::invalid_argument(message);
    }
#if defined(SO_PROTOCOL)
    if (socket_option(socket, SO_PROTOCOL) != protocol) {
        throw std::invalid_argument(message);
    }
#endif
}

void ensure_ipv6_only(native_socket socket, bool ipv6, std::optional<bool> expected) {
    if (!expected) {
        return;
    }
    int value = 0;
    auto length = static_cast<socket_length>(sizeof(value));
    if (!ipv6 || ::getsockopt(os_socket(socket), IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<char*>(&value), &length) != 0 || (value != 0) != *expected) {
        throw std::invalid_argument("inherited listener IPV6_V6ONLY does not match its configuration");
    }
}

}  // namespace

void close_native_socket(native_socket socket) noexcept {
#if defined(_WIN32)
    if (socket != invalid_native_socket) {
        (void)::closesocket(os_socket(socket));
    }
#else
    if (socket >= 0) {
        (void)::close(socket);
    }
#endif
}

void adopt_inherited_tcp_socket(native_socket socket, const asio::ip::tcp::endpoint& endpoint, const char* message, std::optional<bool> ipv6_only) {
    ensure_socket_kind(socket, SOCK_STREAM, IPPROTO_TCP, message);
    if (socket_option(socket, SO_ACCEPTCONN).value_or(0) == 0 || !bound_to(socket, endpoint)) {
        throw std::invalid_argument(message);
    }
    ensure_ipv6_only(socket, endpoint.address().is_v6(), ipv6_only);
    mark_close_on_exec(socket);
}

void adopt_inherited_udp_socket(native_socket socket, const asio::ip::udp::endpoint& endpoint, const char* message, std::optional<bool> ipv6_only) {
    ensure_socket_kind(socket, SOCK_DGRAM, IPPROTO_UDP, message);
    if (!bound_to(socket, endpoint)) {
        throw std::invalid_argument(message);
    }
    ensure_ipv6_only(socket, endpoint.address().is_v6(), ipv6_only);
    mark_close_on_exec(socket);
}

#if !defined(_WIN32)
native_socket duplicate_native_socket(native_socket socket) {
    const int duplicate = ::dup(socket);
    if (duplicate < 0) {
        throw std::system_error(errno, asio::system_category(), "failed to duplicate a listener socket");
    }
    return duplicate;
}
#endif

inherited_listener_sockets::~inherited_listener_sockets() {
    for (const auto socket : tcp_) {
        close_native_socket(socket);
    }
    for (const auto socket : udp_) {
        close_native_socket(socket);
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {

namespace {

// Only the connection's own transport can end it normally. A handler or
// upstream failure is a real fault even when it carries one of the codes
// is_inbound_connection_end() accepts: an upstream request that timed out or
// was reset says nothing about the inbound peer.
[[nodiscard]] bool is_connection_end(const std::exception_ptr& exception) noexcept {
    try {
        std::rethrow_exception(exception);
    } catch (const inbound_transport_error& error) {
        return is_inbound_connection_end(error.code());
    } catch (...) {
        return false;
    }
}

}  // namespace

void connection_failure_sink::invoke(ip_address remote_address, std::exception_ptr exception) const noexcept {
    if (exception == nullptr || is_connection_end(exception)) {
        return;
    }
    if (counter_ != nullptr) {
        counter_->fetch_add(1, std::memory_order_relaxed);
    }
    if (callback_) {
        callback_(connection_failure_record_access::make(remote_address, std::move(exception)));
        return;
    }
    ruvia::report_unhandled_failure("web connection", exception);
}

void connection_failure_sink::invoke(std::string_view remote_address, std::exception_ptr exception) const noexcept {
    invoke(ip_address::parse(remote_address).value_or(ip_address{}), std::move(exception));
}

void http_server_options::set_server(const server_config& config) {
    validate_server_limits(config);
    if (const auto& compression = config.compression_) {
        ensure_positive_size(compression->min_bytes_, "compression minimum size must be greater than zero");
        if (compression->sync_bytes_ < compression->min_bytes_) {
            throw std::invalid_argument(
                "compression synchronous size must not be smaller than the minimum size");
        }
        if (compression->max_bytes_ < compression->sync_bytes_) {
            throw std::invalid_argument(
                "compression maximum size must not be smaller than the synchronous size");
        }
    }
    std::optional<http_cors_policy> cors;
    if (config.cors_) {
        cors.emplace(*config.cors_);
    }
    server_ = config;
    compression_ = config.compression_;
    cors_ = std::move(cors);
}

void http_server_options::set_rate_limit(const rate_limit_config& config) {
    validate_rate_limit_config(config);
    default_rate_limit_per_worker_ = config.rule_;
    rate_limit_capacity_per_worker_ = config.capacity_per_worker_;
}

void http_server_options::set_trusted_proxies(const trusted_proxy_config& config) {
    http_client_ip_config ip_config;
    ip_config.enabled_ = !config.cidrs_.empty();
    ip_config.trusted_proxies_ = config.cidrs_;
    ip_config.forwarding_headers_ = config.forwarding_headers_;
    ip_config.trust_x_forwarded_proto_ = config.trust_x_forwarded_proto_;
    trusted_proxies_ = http_client_ip_extractor(ip_config);
}

http_server_listener_definition validate_run_listener(const http_server_listener_definition& listener,
    const http_server_options& options, std::chrono::steady_clock::time_point key_origin) {
    const auto worker_count = options.server_.worker_count_;
    if (const auto* redirect = std::get_if<http_server_listener_definition::redirect_http_to_https>(&listener.transport_)) {
        detail::ensure_non_zero_port(redirect->https_port_, "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
    }
    if (const auto* tls = std::get_if<http_server_listener_definition::tls>(&listener.transport_)) {
        switch (tls->tcp_mode_) {
            case http_server_listener_definition::tls_tcp_mode::tls_only:
                if (tls->plaintext_redirect_) {
                    throw std::invalid_argument("a plaintext redirect requires a mixed HTTP/HTTPS listener");
                }
                break;
            case http_server_listener_definition::tls_tcp_mode::detect_plain_or_tls:
                if (tls->plaintext_redirect_) {
                    detail::ensure_non_zero_port(
                        tls->plaintext_redirect_->https_port_, "HTTP-to-HTTPS redirect requires a fixed HTTPS listen port");
                }
                break;
            default:
                throw std::invalid_argument("invalid TLS TCP transport mode");
        }
    }
    if (listener.connection_.has_value() && !options.connection_) {
        throw std::invalid_argument("listener_dispatch::connection requires application::on_connection()");
    }
    if (listener.request_stream_ && !options.request_stream_) {
        throw std::invalid_argument("listener_dispatch::request_stream requires application::on_request_stream()");
    }
    // A QUIC connection ID names its owning worker in 16 bits.
    if (listener.http3_.has_value() && worker_count > http3_max_worker_count) {
        throw std::invalid_argument("HTTP/3 supports at most 65536 workers");
    }
#if !defined(_WIN32)
    // Worker i serves inherited socket i; listen() admitted one per service on Windows.
    if (const auto& inherited = listener.inherited_sockets_; inherited != nullptr &&
                                                             ((!inherited->tcp().empty() && inherited->tcp().size() != worker_count) ||
                                                                 (!inherited->udp().empty() && inherited->udp().size() != worker_count))) {
        throw std::invalid_argument("inherited listener sockets must number one per worker");
    }
#endif
    auto validated = listener.clone();
    // One ticket key ring per TLS listener for this run, so TLS resumption
    // does not depend on which worker accepts the connection. Credentials
    // were loaded once by listen(); every worker installs them.
    if (auto* tls = std::get_if<http_server_listener_definition::tls>(&validated.transport_)) {
        tls->session_ticket_keys_ = std::make_shared<tls_session_ticket_key_ring>(key_origin);
    }
    // One QUIC secret per HTTP/3 service, shared by every worker so any
    // worker can tell which one owns a connection ID.
    if (validated.http3_.has_value()) {
        auto secret = std::make_shared<std::array<std::uint8_t, 32>>();
        if (!fill_random_bytes(std::span<unsigned char>(secret->data(), secret->size()))) {
            throw std::runtime_error("failed to generate the HTTP/3 connection ID secret");
        }
        validated.http3_->secret_ = std::move(secret);
    }
    return validated;
}

validated_http_server_configuration::validated_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options)
    : listeners_(),
      options_(std::move(options)),
      // Every listener's key epochs start together, so all workers rotate them
      // at the same instants.
      tls_key_origin_(std::chrono::steady_clock::now()) {
    listeners_.reserve(listeners.size());
    for (const auto& listener : listeners) {
        listeners_.push_back(
            validate_run_listener(listener, options_, tls_key_origin_));
    }
}

namespace {

void validate_worker_queue_capacity(std::size_t capacity) {
    detail::ensure_positive_size(capacity, "worker queue capacity must be greater than zero");
    if (capacity == std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument("worker queue capacity must leave a reserved execution slot");
    }
}

}  // namespace

void validate_server_limits(const server_config& config) {
    detail::ensure_positive_size(config.worker_count_, "worker count must be greater than zero");
    if (config.process_signal_handlers_ != process_signal_handler_policy::external_owner &&
        config.process_signal_handlers_ != process_signal_handler_policy::install) {
        throw std::invalid_argument("process signal handler policy is invalid");
    }
    detail::ensure_positive_optional_durations("configured server timeouts must be greater than zero",
        config.idle_timeout_, config.request_header_timeout_, config.request_body_timeout_,
        config.write_timeout_);
    detail::ensure_positive_duration(
        config.connection_scan_interval_, "connection scan interval must be greater than 0");
    validate_worker_queue_capacity(config.worker_queue_capacity_);
    detail::ensure_positive_size(config.max_buffered_body_bytes_, "buffered body limit must be greater than 0");
    detail::ensure_positive_optional_size(
        config.max_stream_body_bytes_, "configured stream body limit must be greater than zero");
    detail::ensure_positive_size(
        config.max_websocket_message_bytes_, "websocket message limit must be greater than 0");
    detail::ensure_positive_optional_size(
        config.max_connections_per_worker_, "configured connection limit must be greater than zero");
    detail::ensure_positive_optional_size(config.max_requests_per_connection_,
        "configured requests-per-connection limit must be greater than zero");
    detail::ensure_positive_size(config.max_inbound_buffer_bytes_per_worker_,
        "worker inbound buffer budget must be greater than zero");
    detail::ensure_positive_size(config.max_inbound_buffer_bytes_per_connection_,
        "connection inbound buffer budget must be greater than zero");
    detail::ensure_positive_optional_durations("completion deadlines must be greater than zero",
        config.header_completion_timeout_, config.body_completion_timeout_);
    detail::ensure_positive_size(config.http_client_worker_.result_budget_.max_retained_bytes_,
        "HTTP client retained result byte budget must be greater than zero");
    detail::ensure_positive_size(config.http_client_worker_.result_budget_.max_in_flight_bytes_,
        "HTTP client in-flight response budget must be greater than zero");
    detail::ensure_positive_size(config.http_client_worker_.max_concurrent_dns_refreshes_,
        "HTTP client DNS refresh concurrency must be greater than zero");
}

[[nodiscard]] validated_http_server_configuration validate_http_server_configuration(
    std::span<const http_server_listener_definition> listeners, http_server_options&& options) {
    return validated_http_server_configuration(listeners, std::move(options));
}

}  // namespace ruvia::detail

namespace ruvia::detail {
namespace {

using snapshot_owner = std::unique_ptr<static_root>;

// Snapshots are released by whichever thread drops the last reference, so the
// control block and the root both come from the process resource.
[[nodiscard]] document_root_snapshot share_snapshot(snapshot_owner owner) {
    return document_root_snapshot(std::move(owner));
}

}  // namespace

document_root_publisher::document_root_publisher(const std::filesystem::path& root,
    const static_root_config_storage& config, std::chrono::milliseconds interval,
    static_root_precompression_options precompression, static_root_resident_options resident,
    std::size_t worker_count)
    : root_(root),
      interval_(interval),
      precompression_(precompression),
      resident_(resident),
      mailboxes_(worker_count) {
    auto initial = static_root_access::make(root, config);
    static_root_access::install_resident_bodies(*initial, nullptr, resident_);
    static_root_access::install_precompressed_variants(*initial, nullptr, precompression_);
    current_ = share_snapshot(std::move(initial));
}

document_root_publisher::~document_root_publisher() = default;

document_root_publisher::mailbox::~mailbox() {
    // The graph has joined every worker before destroying the mailbox host.
    delete pending_.exchange(nullptr);
}

document_root_publisher::mailbox& document_root_publisher::register_worker() noexcept {
    if (registered_workers_ == mailboxes_.size()) {
        std::terminate();
    }
    return mailboxes_[registered_workers_++];
}

void document_root_publisher::record_failure(std::exception_ptr failure) noexcept {
    failures_.fetch_add(1, std::memory_order_relaxed);
    ruvia::report_unhandled_failure("document root refresh", std::move(failure));
}

void document_root_publisher::publish(document_root_snapshot snapshot) {
    std::vector<publication_owner> publication;
    publication.reserve(mailboxes_.size());
    for (std::size_t i = 0; i < mailboxes_.size(); ++i) {
        publication.push_back(std::make_unique<document_root_snapshot>(snapshot));
    }
    // Every allocation has succeeded before any worker can see this snapshot.
    commit_publication(std::move(snapshot), publication);
}

void document_root_publisher::commit_publication(document_root_snapshot snapshot,
    std::vector<publication_owner>& publication) noexcept {
    current_.swap(snapshot);
    for (std::size_t i = 0; i < mailboxes_.size(); ++i) {
        publication_owner superseded(
            mailboxes_[i].pending_.exchange(publication[i].release(), std::memory_order_acq_rel));
    }
    // Unread nodes are reclaimed by the next publish; adopted snapshots live
    // only as long as a worker's current generation or request leases need them.
}

task<void> document_root_publisher::refresh(
    const worker_ref& worker, const http_server_worker_state& state, const stop_token& stop) {
    // Shared ownership lets the blocking jobs read the previous snapshot while
    // workers keep serving it, and lets resident bytes and precompression
    // reuse its unchanged file versions.
    const auto previous = current_;
    snapshot_owner candidate(nullptr);
    try {
        // The unchanged-snapshot comparison walks every entry, so it runs on
        // the pool with the rebuild; an unchanged root yields no candidate.
        auto rebuilt = co_await ruvia::run_blocking(worker, {.stop_token_ = stop}, [previous, root = root_] {
            auto built = static_root_access::make(root,
                static_root_access::copy_config(*previous));
            if (static_root_access::fingerprint(*built) == static_root_access::fingerprint(*previous) &&
                static_root_access::same_snapshot(*built, *previous)) {
                built.reset();
            }
            return built;
        });
        if (!rebuilt.completed()) {
            // A refresh is transactional: keep serving the last complete
            // index, but expose filesystem/permission failures to metrics
            // instead of silently turning them into stale content. A rejected
            // offload (pool stopping) is not a refresh failure.
            if (rebuilt.failed()) {
                record_failure(rebuilt.error());
            }
            co_return;
        }
        candidate = std::move(rebuilt).take_value();
    } catch (...) {
        // The offload wrapper reports pool shutdown as a status, but creating
        // its one-shot channel or transporting a result can still throw.
        record_failure(std::current_exception());
        co_return;
    }

    if (candidate == nullptr || !http_server_worker_running(state)) {
        co_return;
    }

    if (precompression_.enabled() || resident_.enabled()) {
        try {
            auto prepared = co_await ruvia::run_blocking(worker, {.stop_token_ = stop},
                [candidate = std::move(candidate), previous, precompression = precompression_,
                    resident = resident_]() mutable {
                    static_root_access::install_resident_bodies(*candidate, previous.get(), resident);
                    static_root_access::install_precompressed_variants(
                        *candidate, previous.get(), precompression);
                    return std::move(candidate);
                });
            if (!prepared.completed()) {
                if (prepared.failed()) {
                    record_failure(prepared.error());
                }
                co_return;
            }
            candidate = std::move(prepared).take_value();
        } catch (...) {
            record_failure(std::current_exception());
            co_return;
        }
        if (!http_server_worker_running(state)) {
            co_return;
        }
    }

    try {
        publish(share_snapshot(std::move(candidate)));
    } catch (...) {
        record_failure(std::current_exception());
    }
}

static_root_runtime::static_root_runtime(http_server_options& options, const worker_ref& worker_value,
    const http_server_worker_state& state_value)
    : options_(options),
      publisher_(options.document_root_.publisher()),
      mailbox_(publisher_ == nullptr ? nullptr : &publisher_->register_worker()),
      worker_(worker_value),
      state_(state_value),
      pending_(nullptr),
      current_(nullptr),
      retired_(),
      refreshes_(publisher_ != nullptr && mailbox_ == publisher_->mailboxes_.data()) {
    if (publisher_ != nullptr) {
        // Worker construction is serialized before startup; no publisher can
        // replace this shared_ptr while its initial reference is copied.
        current_ = std::make_unique<document_root_generation>(publisher_->current_);
        options_.document_root_.publish(*current_);
    }
}

static_root_runtime::~static_root_runtime() = default;

void static_root_runtime::adopt_published() {
    if (auto* published = mailbox_->pending_.exchange(nullptr, std::memory_order_acq_rel);
        published != nullptr) {
        pending_.reset(published);
    }
    if (pending_ == nullptr) {
        return;
    }
    // The exchanged node is exclusively ours, so copying its shared_ptr cannot
    // race the publisher reclaiming or replacing the source object.
    auto next = std::make_unique<document_root_generation>(*pending_);
    if (current_->leased()) {
        // If vector growth fails both current_ and pending_ remain owned here;
        // adoption retries next interval, or skips to a newer mailbox node.
        retired_.push_back(std::move(current_));
    }
    current_ = std::move(next);
    options_.document_root_.publish(*current_);
    pending_.reset();
}

task<void> static_root_runtime::refresh(const stop_token& stop) {
    if (publisher_ == nullptr) {
        std::terminate();
    }
    const auto interval = publisher_->interval();
    for (;;) {
        if (!http_server_worker_running(state_)) {
            co_return;
        }
        if (co_await sleep_for(worker_, interval, stop) == timer_sleep_result::stop_requested) {
            co_return;
        }
        if (!http_server_worker_running(state_)) {
            co_return;
        }

        std::erase_if(retired_, [](const generation_owner& generation) { return !generation->leased(); });

        if (refreshes_) {
            co_await publisher_->refresh(worker_, state_, stop);
            if (!http_server_worker_running(state_)) {
                co_return;
            }
        }
        try {
            adopt_published();
        } catch (...) {
            // This worker keeps serving its last generation and retains the
            // exclusively claimed publication node for the next interval.
            publisher_->record_failure(std::current_exception());
        }
    }
}

}  // namespace ruvia::detail

namespace ruvia::detail {

web_worker_runtime::web_worker_runtime(const validated_http_server_configuration& configuration,
    const route_table& routes_value, worker_capability_definitions capabilities, blocking_pool* pool,
    std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_placement http3_placement
#endif
    )
    : web_worker_runtime(configuration.listeners(), routes_value, capabilities, configuration.options(), pool,
          worker_index
#if defined(RUVIA_ENABLE_HTTP3)
          ,
          http3_placement
#endif
      ) {
}

web_worker_runtime::web_worker_runtime(std::span<const http_server_listener_definition> listeners,
    const route_table& routes_value, worker_capability_definitions capabilities,
    http_server_options validated_options, blocking_pool* pool, std::size_t worker_index
#if defined(RUVIA_ENABLE_HTTP3)
    ,
    http3_worker_placement http3_placement
#endif
    )
    : runtime_({.queue_capacity_ = validated_options.server_.worker_queue_capacity_,
          .io_policy_ = ruvia::worker_io_policy::single_owner,
          .blocking_pool_ = pool}),
      io_context_(runtime_.io_context()),
      finalize_signal_(runtime_.worker()),
      routes_(routes_value),
      inbound_budget_(validated_options.server_.max_inbound_buffer_bytes_per_worker_),
      background_tasks_(runtime_.worker()),
      options_(std::move(validated_options)),
      static_roots_(options_, runtime_.worker(), worker_state_),
      capabilities_(io_context_, runtime_.worker(), capabilities,
          worker_capability_options{
              .default_rate_limit_ = options_.default_rate_limit_per_worker_,
              .route_rate_limits_ = routes_.has_route_rate_limit() ? route_rate_limit_presence::present
                                                                   : route_rate_limit_presence::absent,
              .rate_limit_capacity_ = options_.rate_limit_capacity_per_worker_,
              .max_decoded_body_bytes_ = options_.server_.max_buffered_body_bytes_,
              .http_client_worker_ = options_.server_.http_client_worker_,
              .env_ = options_.env_,
              .trusted_proxies_ =
                  options_.trusted_proxies_.empty() ? nullptr : &options_.trusted_proxies_,
              .precompressed_static_files_ = options_.compression_.has_value(),
              .file_io_timeout_ = options_.server_.write_timeout_,
              .connection_failure_ = &options_.connection_failure_,
          }),
      connections_(io_context_, runtime_.worker(), routes_, capabilities_, options_, stop_token_, worker_state_, background_tasks_, listeners,
          worker_failure_sink{.target_ = &runtime_,
              .invoke_ = [](void* target, const std::exception_ptr& failure) noexcept {
                  (void)static_cast<ruvia::worker_runtime*>(target)->fail(failure);
              }},
          worker_index
#if defined(RUVIA_ENABLE_HTTP3)
          ,
          http3_placement
#endif
          ),
      web_worker_dispatch_(std::make_shared<web_worker_dispatch>(runtime_, capabilities_, background_tasks_, stop_token_)) {
    options_.inbound_budget_ = &inbound_budget_;
    options_.codecs_ = &codecs_;
    runtime_.configure({
        .startup_ = [this] {
            worker_state_ = http_server_worker_state::running;
            // Startup runs before the context does; the task starts in its first turn.
            asio::post(io_context_, [this] {
                ruvia::async_start(run_worker(), asio::bind_executor(io_context_, [this](std::exception_ptr failure) noexcept {
                    if (failure) {
                        (void)runtime_.fail(failure);
                    }
                }));
            }); },
        .serve_ = [this] { serve_on_context(); },
        .stop_admission_ = [this] { stop_admission_on_context(); },
        .finalize_ = [this] { finalize_on_context(); },
        .failure_ = [this](std::exception_ptr failure) noexcept {
            worker_failures_.fetch_add(1, std::memory_order_relaxed);
            options_.failure_.notify(failure); },
        .shutdown_ = [this]() noexcept {
            connections_.retire_tls();
            worker_state_ = http_server_worker_state::stopped; },
    });
}

web_worker_runtime::~web_worker_runtime() {
    runtime_.request_stop();
    runtime_.finalize();
    runtime_.join();
}

http_server_stats web_worker_runtime::stats() const {
    auto stats = connections_.stats();
    stats.worker_failures_ = worker_failures_.load(std::memory_order_relaxed);
    return stats;
}

web_worker_ref web_worker_runtime::web_worker() const {
    return web_worker_dispatch::make_ref(web_worker_dispatch_);
}

void web_worker_runtime::serve_on_context() {
    if (static_roots_.enabled()) {
        background_tasks_.spawn(static_roots_.refresh(stop_token_));
    }
    connections_.open_admission();
}

void web_worker_runtime::stop_admission_on_context() noexcept {
    worker_state_ = http_server_worker_state::stopped;
    stop_source_.request_stop();
    connections_.stop();
    runtime_.finalize();
}

void web_worker_runtime::finalize_on_context() noexcept {
    capabilities_.close_now();
    finalize_signal_.notify();
}

task<void> web_worker_runtime::run_worker() {
    try {
        // A stop applied before this task first runs has already stopped the
        // worker timers; starting the scanner or capability I/O would fail.
        if (!stop_token_.stop_requested()) {
            connections_.prepare();
            co_await capabilities_.start();
            // Ignored once stopping: core has already aborted the barrier.
            runtime_.mark_ready();
        }
    } catch (...) {
        // A stop during startup closes the capabilities whose connect is still
        // pending; that abort is the requested cancellation, not a failure.
        if (!stop_token_.stop_requested()) {
            (void)runtime_.fail(std::current_exception());
        }
    }
    // Retain worker-local state until finalization joins sessions and pending
    // listener operations on this same owner.
    co_await finalize_signal_.wait();
    try {
        co_await background_tasks_.join();
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
    try {
        co_await capabilities_.stop();
    } catch (...) {
        (void)runtime_.fail(std::current_exception());
    }
}

}  // namespace ruvia::detail
