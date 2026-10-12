#include "tls/tls_credential.h"

#include <memory>
#include <new>
#include <utility>

#include <asio/error.hpp>
#include <asio/ssl/error.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "tls/tls_password_scope.h"

namespace ruvia::detail {
namespace {

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

}  // namespace

tls_credential_error take_tls_credential_error(const char* operation) {
    const auto code = ::ERR_get_error();
    return {.operation_ = operation,
        .error_ = ERR_SYSTEM_ERROR(code) ? std::error_code(ERR_GET_REASON(code), asio::error::get_system_category())
                                         : std::error_code(static_cast<int>(code), asio::error::get_ssl_category())};
}

std::optional<tls_certificate> tls_certificate::load(const char* certificate_chain_file, const char* private_key_file,
    std::string_view private_key_password, tls_credential_error& error) {
    // OpenSSL's PEM loaders resolve the full chain and the (possibly
    // encrypted) key; a scratch context keeps that one authoritative parser.
    ::ERR_clear_error();
    const std::unique_ptr<SSL_CTX, ssl_context_deleter> scratch(SSL_CTX_new(TLS_method()));
    if (scratch == nullptr) {
        error = take_tls_credential_error("SSL_CTX_new");
        return std::nullopt;
    }
    {
        const tls_password_scope password_scope(*scratch, private_key_password);
        ::ERR_clear_error();
        if (::SSL_CTX_use_certificate_chain_file(scratch.get(), certificate_chain_file) != 1) {
            error = take_tls_credential_error("use_certificate_chain_file");
            return std::nullopt;
        }
        ::ERR_clear_error();
        if (::SSL_CTX_use_PrivateKey_file(scratch.get(), private_key_file, SSL_FILETYPE_PEM) != 1) {
            error = take_tls_credential_error("use_private_key_file");
            return std::nullopt;
        }
    }
    ::ERR_clear_error();
    if (::SSL_CTX_check_private_key(scratch.get()) != 1) {
        error = take_tls_credential_error("check_private_key");
        return std::nullopt;
    }
    STACK_OF(X509)* chain = nullptr;
    if (SSL_CTX_get0_chain_certs(scratch.get(), &chain) != 1) {
        error = take_tls_credential_error("get0_chain_certs");
        return std::nullopt;
    }
    return tls_certificate(SSL_CTX_get0_certificate(scratch.get()), chain, SSL_CTX_get0_privatekey(scratch.get()));
}

tls_certificate::tls_certificate(X509* certificate, STACK_OF(X509) * chain, EVP_PKEY* private_key) {
    if (chain != nullptr) {
        chain_ = X509_chain_up_ref(chain);
        if (chain_ == nullptr) {
            throw std::bad_alloc();
        }
    }
    X509_up_ref(certificate);
    certificate_ = certificate;
    EVP_PKEY_up_ref(private_key);
    private_key_ = private_key;
}

tls_certificate::tls_certificate(const tls_certificate& other)
    : tls_certificate(other.certificate_, other.chain_, other.private_key_) {}

tls_certificate::tls_certificate(tls_certificate&& other) noexcept
    : certificate_(std::exchange(other.certificate_, nullptr)),
      chain_(std::exchange(other.chain_, nullptr)),
      private_key_(std::exchange(other.private_key_, nullptr)) {}

tls_certificate& tls_certificate::operator=(const tls_certificate& other) {
    if (this != &other) {
        *this = tls_certificate(other);
    }
    return *this;
}

tls_certificate& tls_certificate::operator=(tls_certificate&& other) noexcept {
    if (this != &other) {
        release();
        certificate_ = std::exchange(other.certificate_, nullptr);
        chain_ = std::exchange(other.chain_, nullptr);
        private_key_ = std::exchange(other.private_key_, nullptr);
    }
    return *this;
}

tls_certificate::~tls_certificate() {
    release();
}

void tls_certificate::release() noexcept {
    X509_free(certificate_);
    OSSL_STACK_OF_X509_free(chain_);
    EVP_PKEY_free(private_key_);
    certificate_ = nullptr;
    chain_ = nullptr;
    private_key_ = nullptr;
}

bool tls_certificate::install(SSL_CTX& context, tls_credential_error& error) const {
    ::ERR_clear_error();
    if (::SSL_CTX_use_certificate(&context, certificate_) != 1) {
        error = take_tls_credential_error("use_certificate");
        return false;
    }
    ::ERR_clear_error();
    if (::SSL_CTX_use_PrivateKey(&context, private_key_) != 1) {
        error = take_tls_credential_error("use_private_key");
        return false;
    }
    ::ERR_clear_error();
    if (chain_ != nullptr && SSL_CTX_set1_chain(&context, chain_) != 1) {
        error = take_tls_credential_error("set1_chain");
        return false;
    }
    ::ERR_clear_error();
    if (::SSL_CTX_check_private_key(&context) != 1) {
        error = take_tls_credential_error("check_private_key");
        return false;
    }
    return true;
}

std::optional<tls_trust_store> tls_trust_store::load_file(const char* file, tls_credential_error& error) {
    ::ERR_clear_error();
    std::unique_ptr<X509_STORE, x509_store_deleter> store(X509_STORE_new());
    if (store == nullptr) {
        error = take_tls_credential_error("X509_STORE_new");
        return std::nullopt;
    }
    if (X509_STORE_load_file(store.get(), file) != 1) {
        error = take_tls_credential_error("load_verify_file");
        return std::nullopt;
    }
    return tls_trust_store(store.release());
}

std::optional<tls_trust_store> tls_trust_store::load_default(tls_credential_error& error) {
    ::ERR_clear_error();
    std::unique_ptr<X509_STORE, x509_store_deleter> store(X509_STORE_new());
    if (store == nullptr) {
        error = take_tls_credential_error("X509_STORE_new");
        return std::nullopt;
    }
    if (X509_STORE_set_default_paths(store.get()) != 1) {
        error = take_tls_credential_error("set_default_verify_paths");
        return std::nullopt;
    }
    return tls_trust_store(store.release());
}

tls_trust_store::tls_trust_store(const tls_trust_store& other)
    : store_(other.store_) {
    if (store_ != nullptr && X509_STORE_up_ref(store_) != 1) {
        store_ = nullptr;
        throw std::bad_alloc();
    }
}

tls_trust_store::tls_trust_store(tls_trust_store&& other) noexcept
    : store_(std::exchange(other.store_, nullptr)) {}

tls_trust_store& tls_trust_store::operator=(const tls_trust_store& other) {
    if (this != &other) {
        *this = tls_trust_store(other);
    }
    return *this;
}

tls_trust_store& tls_trust_store::operator=(tls_trust_store&& other) noexcept {
    if (this != &other) {
        X509_STORE_free(store_);
        store_ = std::exchange(other.store_, nullptr);
    }
    return *this;
}

tls_trust_store::~tls_trust_store() {
    X509_STORE_free(store_);
}

void tls_trust_store::install(SSL_CTX& context) const noexcept {
    SSL_CTX_set1_cert_store(&context, store_);
}

}  // namespace ruvia::detail
