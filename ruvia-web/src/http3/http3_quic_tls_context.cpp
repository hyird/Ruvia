#include "http3/http3_quic_tls_context.h"

#include <stdexcept>
#include <utility>

#include <openssl/ssl.h>

#include "ruvia/http/http_ascii.h"

#include "server/http_server_tls_identity.h"

namespace ruvia::detail {
namespace {
// OpenSSL skips ALPN selection when the extension is absent. RFC 9001 Section 8.1
// requires closing immediately with no_application_protocol, before the server flight.
int require_alpn_offer(SSL* ssl, int* alert, void*) noexcept {
    if (SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_application_layer_protocol_negotiation,
            nullptr, nullptr) == 1) {
        return SSL_CLIENT_HELLO_SUCCESS;
    }
    *alert = SSL_AD_NO_APPLICATION_PROTOCOL;
    return SSL_CLIENT_HELLO_ERROR;
}

void configure_quic_tls_context(SSL_CTX* context_value,
    const http_server_listener_definition::tls_identity_type& identity,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy_type>&
        client_certificates,
    int (*alpn_callback)(SSL*, const unsigned char**, unsigned char*, const unsigned char*,
        unsigned int, void*) noexcept) {
    if (context_value == nullptr) {
        throw std::runtime_error("failed to create QUIC TLS context");
    }
    if (SSL_CTX_set_min_proto_version(context_value, TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(context_value, TLS1_3_VERSION) != 1) {
        throw std::runtime_error("failed to configure QUIC TLS 1.3");
    }
    SSL_CTX_set_options(context_value, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_alpn_select_cb(context_value, alpn_callback, nullptr);
    SSL_CTX_set_client_hello_cb(context_value, &require_alpn_offer, nullptr);
    configure_http_server_tls_identity(context_value, identity, client_certificates);
    // Password callback data belongs to the input configuration; credentials are now loaded.
    SSL_CTX_set_default_passwd_cb(context_value, nullptr);
    SSL_CTX_set_default_passwd_cb_userdata(context_value, nullptr);
}

void configure_identity_context(SSL_CTX* context_value,
    const http_server_listener_definition::tls_identity_type& identity) {
    if (context_value == nullptr) {
        throw std::runtime_error("failed to create QUIC SNI identity context");
    }
    configure_http_server_tls_identity(context_value, identity, std::nullopt);
    // Do not retain the configuration's password storage after preloading the key.
    SSL_CTX_set_default_passwd_cb(context_value, nullptr);
    SSL_CTX_set_default_passwd_cb_userdata(context_value, nullptr);
}

}  // namespace

http3_quic_tls_context::http3_quic_tls_context(const http_server_listener_definition::tls_type& tls,
    std::pmr::memory_resource* resource)
    : identity_contexts_(pmr_resource_or_default(resource)),
      sni_identities_(pmr_resource_or_default(resource)) {
    const auto* const method = TLS_method();
    identity_contexts_.reserve(tls.sni_identities_.size());
    sni_identities_.reserve(tls.sni_identities_.size());

    for (const auto& configured : tls.sni_identities_) {
        context_owner identity_context(SSL_CTX_new(method));
        configure_identity_context(identity_context.get(), configured.identity_);
        SSL_CTX* const raw_context = identity_context.get();
        X509* const certificate = SSL_CTX_get0_certificate(raw_context);
        EVP_PKEY* const private_key = SSL_CTX_get0_privatekey(raw_context);
        STACK_OF(X509)* chain = nullptr;
        if (certificate == nullptr || private_key == nullptr ||
            SSL_CTX_get0_chain_certs(raw_context, &chain) != 1) {
            throw std::runtime_error("failed to preload QUIC SNI certificate identity");
        }
        identity_contexts_.push_back(std::move(identity_context));
        sni_identities_.push_back(
            sni_identity{std::pmr::string(configured.host_, sni_identities_.get_allocator().resource()),
                raw_context, certificate, private_key, chain});
    }

    context_owner default_context(SSL_CTX_new(method));
    configure_quic_tls_context(default_context.get(), tls.identity_, tls.client_certificates_,
        &select_alpn_protocol);
    // The listener's process-wide ticket keys: a QUIC resumption ticket issued
    // by one worker is accepted by every other worker of this listener.
    install_tls_session_ticket_keys(*default_context.get(), tls);
    default_context_ = default_context.get();
    early_data_enabled_ = tls.http3_early_data_;
    if (SSL_CTX_set_max_early_data(default_context_,
            early_data_enabled_ ? 65536U : 0U) != 1) {
        throw std::runtime_error("failed to configure QUIC server early-data allowance");
    }
    // Keep OpenSSL's built-in anti-replay enabled. Early data is never enabled
    // for listeners requesting client certificates (validated above).
    SSL_CTX_set_cert_cb(default_context_, &select_certificate, this);
    // SSL objects and active QUIC connections borrow this context's cert callback
    // argument, and SNI cert_cb copies from identity_contexts_; all must stay alive.
    default_context_owner_ = std::move(default_context);
}

http3_quic_tls_context::~http3_quic_tls_context() = default;

void http3_quic_tls_context::context_deleter::operator()(SSL_CTX* context_value) const noexcept {
    SSL_CTX_free(context_value);
}

int http3_quic_tls_context::select_certificate(SSL* ssl, void* argument) noexcept {
    if (ssl == nullptr || argument == nullptr) {
        return 0;
    }
    const char* const server_name = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    if (server_name == nullptr) {
        return 1;
    }

    const auto& owner_value = *static_cast<const http3_quic_tls_context*>(argument);
    for (const auto& entry : owner_value.sni_identities_) {
        if (!http_ascii_equals_ignore_case(entry.host_, server_name)) {
            continue;
        }

        // The certificate, key, and chain are borrowed from entry.context_, which
        // remains owned for the lifetime of this callback. OpenSSL retains what
        // it needs on the SSL object; the helper context continues owning the chain.
        return SSL_use_cert_and_key(
                   ssl, entry.certificate_, entry.private_key_, entry.chain_, 1) == 1
                   ? 1
                   : 0;
    }
    return 1;
}

int http3_quic_tls_context::select_alpn_protocol(SSL*, const unsigned char** output,
    unsigned char* output_length, const unsigned char* input, unsigned int input_length,
    void*) noexcept {
    constexpr unsigned char h3[] = {'h', '3'};
    for (unsigned int offset = 0; offset < input_length;) {
        const unsigned int length = input[offset++];
        if (length > input_length - offset) {
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        if (length == sizeof(h3) && input[offset] == h3[0] && input[offset + 1] == h3[1]) {
            *output = input + offset;
            *output_length = static_cast<unsigned char>(sizeof(h3));
            return SSL_TLSEXT_ERR_OK;
        }
        offset += length;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

}  // namespace ruvia::detail
