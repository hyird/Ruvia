#include "server/http_server_tls_identity.h"

#include <memory>
#include <stdexcept>

#include <asio/error.hpp>
#include <asio/ssl/verify_mode.hpp>
#include <asio/system_error.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "server/http_server_tls_verify.h"
#include "tls/tls_file_paths.h"
#include "tls/tls_password_scope.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] asio::error_code translate_open_ssl_error(unsigned long error) {
    if (ERR_SYSTEM_ERROR(error)) {
        return asio::error_code(ERR_GET_REASON(error), asio::error::get_system_category());
    }
    return asio::error_code(static_cast<int>(error), asio::error::get_ssl_category());
}

[[noreturn]] void throw_tls_identity_error(const char* operation) {
    throw asio::system_error(translate_open_ssl_error(::ERR_get_error()), operation);
}

struct ssl_context_deleter final {
    void operator()(SSL_CTX* context_value) const noexcept {
        SSL_CTX_free(context_value);
    }
};

struct x509_store_deleter final {
    void operator()(X509_STORE* store) const noexcept {
        X509_STORE_free(store);
    }
};

[[nodiscard]] http_server_tls_certificate load_http_server_tls_certificate(
    const http_server_listener_definition::tls_identity_type& identity) {
    validate_http_server_tls_identity(identity);
    // OpenSSL's PEM loaders resolve the full chain and the (possibly
    // encrypted) key; a scratch context keeps that one authoritative parser.
    const std::unique_ptr<SSL_CTX, ssl_context_deleter> scratch(SSL_CTX_new(TLS_method()));
    if (scratch == nullptr) {
        throw_tls_identity_error("SSL_CTX_new");
    }
    {
        const tls_password_scope password_scope(*scratch, identity.private_key_password_);
        ::ERR_clear_error();
        if (::SSL_CTX_use_certificate_chain_file(scratch.get(), identity.certificate_chain_file_.c_str()) != 1) {
            throw_tls_identity_error("use_certificate_chain_file");
        }
        ::ERR_clear_error();
        if (::SSL_CTX_use_PrivateKey_file(scratch.get(), identity.private_key_file_.c_str(), SSL_FILETYPE_PEM) != 1) {
            throw_tls_identity_error("use_private_key_file");
        }
    }
    ::ERR_clear_error();
    if (::SSL_CTX_check_private_key(scratch.get()) != 1) {
        throw_tls_identity_error("check_private_key");
    }
    STACK_OF(X509)* chain = nullptr;
    if (SSL_CTX_get0_chain_certs(scratch.get(), &chain) != 1) {
        throw_tls_identity_error("get0_chain_certs");
    }
    return http_server_tls_certificate(
        SSL_CTX_get0_certificate(scratch.get()), chain, SSL_CTX_get0_privatekey(scratch.get()));
}

[[nodiscard]] http_server_tls_client_ca load_http_server_tls_client_ca(
    const http_server_listener_definition::tls_client_certificate_policy_type& policy) {
    validate_http_server_tls_client_certificate_policy(policy);
    const std::unique_ptr<X509_STORE, x509_store_deleter> store(X509_STORE_new());
    if (store == nullptr) {
        throw_tls_identity_error("X509_STORE_new");
    }
    ::ERR_clear_error();
    if (X509_STORE_load_file(store.get(), policy.verify_file_.c_str()) != 1) {
        throw_tls_identity_error("load_verify_file");
    }
    return http_server_tls_client_ca(store.get());
}

}  // namespace

http_server_tls_certificate::http_server_tls_certificate(
    X509* certificate, STACK_OF(X509) * chain, EVP_PKEY* private_key) {
    if (certificate == nullptr || private_key == nullptr) {
        throw std::invalid_argument("TLS certificate identity is incomplete");
    }
    if (chain != nullptr) {
        chain_ = X509_chain_up_ref(chain);
        if (chain_ == nullptr) {
            throw_tls_identity_error("X509_chain_up_ref");
        }
    }
    X509_up_ref(certificate);
    certificate_ = certificate;
    EVP_PKEY_up_ref(private_key);
    private_key_ = private_key;
}

http_server_tls_certificate::http_server_tls_certificate(const http_server_tls_certificate& other)
    : http_server_tls_certificate(other.certificate_, other.chain_, other.private_key_) {}

http_server_tls_certificate& http_server_tls_certificate::operator=(const http_server_tls_certificate& other) {
    if (this != &other) {
        http_server_tls_certificate copy(other);
        release();
        certificate_ = std::exchange(copy.certificate_, nullptr);
        chain_ = std::exchange(copy.chain_, nullptr);
        private_key_ = std::exchange(copy.private_key_, nullptr);
    }
    return *this;
}

http_server_tls_certificate::~http_server_tls_certificate() {
    release();
}

void http_server_tls_certificate::release() noexcept {
    X509_free(certificate_);
    OSSL_STACK_OF_X509_free(chain_);
    EVP_PKEY_free(private_key_);
    certificate_ = nullptr;
    chain_ = nullptr;
    private_key_ = nullptr;
}

void http_server_tls_certificate::install(SSL_CTX& context) const {
    ::ERR_clear_error();
    if (::SSL_CTX_use_certificate(&context, certificate_) != 1) {
        throw_tls_identity_error("use_certificate");
    }
    ::ERR_clear_error();
    if (::SSL_CTX_use_PrivateKey(&context, private_key_) != 1) {
        throw_tls_identity_error("use_private_key");
    }
    ::ERR_clear_error();
    if (chain_ != nullptr && SSL_CTX_set1_chain(&context, chain_) != 1) {
        throw_tls_identity_error("set1_chain");
    }
    ::ERR_clear_error();
    if (::SSL_CTX_check_private_key(&context) != 1) {
        throw_tls_identity_error("check_private_key");
    }
}

http_server_tls_client_ca::http_server_tls_client_ca(X509_STORE* store) {
    if (store == nullptr || X509_STORE_up_ref(store) != 1) {
        throw std::invalid_argument("TLS client certificate store is unavailable");
    }
    store_ = store;
}

http_server_tls_client_ca::http_server_tls_client_ca(const http_server_tls_client_ca& other)
    : http_server_tls_client_ca(other.store_) {}

http_server_tls_client_ca& http_server_tls_client_ca::operator=(const http_server_tls_client_ca& other) {
    if (this != &other) {
        http_server_tls_client_ca copy(other);
        X509_STORE_free(store_);
        store_ = std::exchange(copy.store_, nullptr);
    }
    return *this;
}

http_server_tls_client_ca::~http_server_tls_client_ca() {
    X509_STORE_free(store_);
}

void validate_http_server_tls_identity(
    const http_server_listener_definition::tls_identity_type& identity) {
    if (identity.certificate_chain_file_.empty() || identity.private_key_file_.empty()) {
        throw std::invalid_argument(
            "TLS certificate chain and private key files must not be empty");
    }
    validate_tls_file_paths({identity.certificate_chain_file_, identity.private_key_file_});
}

void validate_http_server_tls_client_certificate_policy(
    const http_server_listener_definition::tls_client_certificate_policy_type& policy) {
    switch (policy.requirement_) {
        case tls_client_certificate_requirement::optional:
        case tls_client_certificate_requirement::required:
            break;
        default:
            throw std::invalid_argument("TLS client certificate requirement is invalid");
    }
    if (policy.verify_file_.empty()) {
        throw std::invalid_argument("TLS client certificate CA bundle must not be empty");
    }
    validate_tls_file_paths({policy.verify_file_});
}

void load_http_server_tls_credentials(http_server_listener_definition::tls_type& tls) {
    tls.identity_.certificate_ = load_http_server_tls_certificate(tls.identity_);
    for (auto& sni : tls.sni_identities_) {
        sni.identity_.certificate_ = load_http_server_tls_certificate(sni.identity_);
    }
    if (tls.client_certificates_.has_value()) {
        tls.client_certificates_->client_ca_ = load_http_server_tls_client_ca(*tls.client_certificates_);
    }
}

void configure_http_server_tls_identity(SSL_CTX* context_value,
    const http_server_listener_definition::tls_identity_type& identity,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy_type>&
        client_certificates) {
    if (context_value == nullptr) {
        throw std::invalid_argument("TLS context must not be null");
    }
    // Only validated configurations carry loaded credentials; an unvalidated
    // definition must never reach a serving TLS context.
    if (!identity.certificate_.has_value() ||
        (client_certificates.has_value() && !client_certificates->client_ca_.has_value())) {
        throw std::logic_error("TLS listener credentials were not loaded");
    }
    identity.certificate_->install(*context_value);

    if (!client_certificates.has_value()) {
        return;
    }
    SSL_CTX_set1_cert_store(context_value, client_certificates->client_ca_->store());
    // A server that verifies peers without a session ID context turns every
    // resumption attempt into a fatal handshake error (SSL_CTX_set_session_id_context(3)).
    static constexpr unsigned char session_id_context[] = {'r', 'u', 'v', 'i', 'a'};
    ::ERR_clear_error();
    if (::SSL_CTX_set_session_id_context(
            context_value, session_id_context, sizeof(session_id_context)) != 1) {
        throw_tls_identity_error("set_session_id_context");
    }
    SSL_CTX_set_verify(context_value,
        static_cast<int>(http_server_tls_verify_mode(client_certificates->requirement_)), nullptr);
}

}  // namespace ruvia::detail
