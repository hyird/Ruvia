#include "tls/tls_credential.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

#include <asio/error.hpp>
#include <asio/ssl/error.hpp>
#include <asio/system_error.hpp>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

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

struct bio_deleter final {
    void operator()(BIO* bio) const noexcept {
        BIO_free(bio);
    }
};

struct x509_deleter final {
    void operator()(X509* certificate) const noexcept {
        X509_free(certificate);
    }
};

struct private_key_deleter final {
    void operator()(EVP_PKEY* private_key) const noexcept {
        EVP_PKEY_free(private_key);
    }
};

// A read-only BIO over caller-owned PEM text, which it never copies.
[[nodiscard]] std::unique_ptr<BIO, bio_deleter> open_pem_text(std::string_view text) {
    if (!std::in_range<int>(text.size())) {
        throw std::invalid_argument("TLS PEM text is too large");
    }
    ::ERR_clear_error();
    std::unique_ptr<BIO, bio_deleter> input(BIO_new_mem_buf(text.data(), static_cast<int>(text.size())));
    if (input == nullptr) {
        throw_tls_credential_error("BIO_new_mem_buf");
    }
    return input;
}

// SSL_CTX_use_certificate_chain_file() over memory: the first certificate is
// the leaf, every later one joins its chain, and only the end of the text
// ends the chain.
void use_certificate_chain_pem(SSL_CTX& context, std::string_view text) {
    const auto input = open_pem_text(text);
    auto* const password_callback = SSL_CTX_get_default_passwd_cb(&context);
    auto* const password_userdata = SSL_CTX_get_default_passwd_cb_userdata(&context);
    const std::unique_ptr<X509, x509_deleter> leaf(
        PEM_read_bio_X509_AUX(input.get(), nullptr, password_callback, password_userdata));
    if (leaf == nullptr || SSL_CTX_use_certificate(&context, leaf.get()) != 1) {
        throw_tls_credential_error("use_certificate_chain_pem");
    }
    for (;;) {
        std::unique_ptr<X509, x509_deleter> issuer(
            PEM_read_bio_X509(input.get(), nullptr, password_callback, password_userdata));
        if (issuer == nullptr) {
            break;
        }
        if (SSL_CTX_add0_chain_cert(&context, issuer.get()) != 1) {
            throw_tls_credential_error("use_certificate_chain_pem");
        }
        // The context owns it now.
        (void)issuer.release();
    }
    const auto error = ::ERR_peek_last_error();
    if (ERR_GET_LIB(error) != ERR_LIB_PEM || ERR_GET_REASON(error) != PEM_R_NO_START_LINE) {
        throw_tls_credential_error("use_certificate_chain_pem");
    }
    ::ERR_clear_error();
}

void use_private_key_pem(SSL_CTX& context, std::string_view text) {
    const auto input = open_pem_text(text);
    const std::unique_ptr<EVP_PKEY, private_key_deleter> private_key(PEM_read_bio_PrivateKey(input.get(), nullptr,
        SSL_CTX_get_default_passwd_cb(&context), SSL_CTX_get_default_passwd_cb_userdata(&context)));
    if (private_key == nullptr || SSL_CTX_use_PrivateKey(&context, private_key.get()) != 1) {
        throw_tls_credential_error("use_private_key_pem");
    }
}

// Borrow the configured password only during synchronous credential loading.
// An absent password uses a caller provider, or an empty noninteractive default.
class tls_password_scope final {
public:
    tls_password_scope(SSL_CTX& context_value, std::string_view password) noexcept
        : context_(&context_value),
          password_(password),
          previous_callback_(SSL_CTX_get_default_passwd_cb(&context_value)),
          previous_userdata_(SSL_CTX_get_default_passwd_cb_userdata(&context_value)) {
        const bool default_source = previous_userdata_ == nullptr &&
                                    (previous_callback_ == nullptr || previous_callback_ == PEM_def_callback);
        if (!password.empty() || default_source) {
            SSL_CTX_set_default_passwd_cb(context_, &copy_password);
            SSL_CTX_set_default_passwd_cb_userdata(context_, this);
        }
    }

    ~tls_password_scope() {
        // The caller may own its userdata: Asio deletes its callback object.
        SSL_CTX_set_default_passwd_cb(context_, previous_callback_);
        SSL_CTX_set_default_passwd_cb_userdata(context_, previous_userdata_);
    }

    tls_password_scope(const tls_password_scope&) = delete;
    tls_password_scope& operator=(const tls_password_scope&) = delete;
    tls_password_scope(tls_password_scope&&) = delete;
    tls_password_scope& operator=(tls_password_scope&&) = delete;

private:
    static int copy_password(char* buffer, int size, int, void* userdata) noexcept {
        if (buffer == nullptr || size <= 0 || userdata == nullptr) {
            return -1;
        }
        const auto password = static_cast<const tls_password_scope*>(userdata)->password_;
        if (password.size() > static_cast<std::size_t>(size)) {
            return -1;
        }
        if (!password.empty()) {
            std::memcpy(buffer, password.data(), password.size());
        }
        return static_cast<int>(password.size());
    }

    SSL_CTX* context_;
    std::string_view password_;
    pem_password_cb* previous_callback_;
    void* previous_userdata_;
};

// OpenSSL's PEM loaders resolve the full chain and the (possibly encrypted)
// key into a scratch context, which keeps that one authoritative parser.
[[nodiscard]] std::unique_ptr<SSL_CTX, ssl_context_deleter> make_scratch_context() {
    ::ERR_clear_error();
    std::unique_ptr<SSL_CTX, ssl_context_deleter> scratch(SSL_CTX_new(TLS_method()));
    if (scratch == nullptr) {
        throw_tls_credential_error("SSL_CTX_new");
    }
    return scratch;
}

}  // namespace

void throw_tls_credential_error(const char* operation) {
    const auto code = ::ERR_get_error();
    const auto error = ERR_SYSTEM_ERROR(code)
                           ? std::error_code(ERR_GET_REASON(code), asio::error::get_system_category())
                           : std::error_code(static_cast<int>(code), asio::error::get_ssl_category());
    throw asio::system_error(error, operation);
}

tls_certificate tls_certificate::load(const char* certificate_chain_file, const char* private_key_file,
    std::string_view private_key_password) {
    const auto scratch = make_scratch_context();
    {
        const tls_password_scope password_scope(*scratch, private_key_password);
        ::ERR_clear_error();
        if (::SSL_CTX_use_certificate_chain_file(scratch.get(), certificate_chain_file) != 1) {
            throw_tls_credential_error("use_certificate_chain_file");
        }
        ::ERR_clear_error();
        if (::SSL_CTX_use_PrivateKey_file(scratch.get(), private_key_file, SSL_FILETYPE_PEM) != 1) {
            throw_tls_credential_error("use_private_key_file");
        }
    }
    return adopt_loaded(*scratch);
}

tls_certificate tls_certificate::load_pem(std::string_view certificate_chain, std::string_view private_key,
    std::string_view private_key_password) {
    const auto scratch = make_scratch_context();
    {
        const tls_password_scope password_scope(*scratch, private_key_password);
        use_certificate_chain_pem(*scratch, certificate_chain);
        use_private_key_pem(*scratch, private_key);
    }
    return adopt_loaded(*scratch);
}

tls_certificate tls_certificate::adopt_loaded(SSL_CTX& scratch) {
    ::ERR_clear_error();
    if (::SSL_CTX_check_private_key(&scratch) != 1) {
        throw_tls_credential_error("check_private_key");
    }
    STACK_OF(X509)* chain = nullptr;
    if (SSL_CTX_get0_chain_certs(&scratch, &chain) != 1) {
        throw_tls_credential_error("get0_chain_certs");
    }
    return tls_certificate(SSL_CTX_get0_certificate(&scratch), chain, SSL_CTX_get0_privatekey(&scratch));
}

tls_certificate::tls_certificate(X509* certificate, STACK_OF(X509) * chain, EVP_PKEY* private_key) {
    if (certificate == nullptr || private_key == nullptr) {
        throw std::invalid_argument("TLS certificate and private key must not be null");
    }
    if (chain != nullptr) {
        chain_ = X509_chain_up_ref(chain);
        if (chain_ == nullptr) {
            throw std::bad_alloc();
        }
    }
    if (X509_up_ref(certificate) != 1) {
        OSSL_STACK_OF_X509_free(chain_);
        chain_ = nullptr;
        throw std::bad_alloc();
    }
    certificate_ = certificate;
    if (EVP_PKEY_up_ref(private_key) != 1) {
        X509_free(certificate_);
        certificate_ = nullptr;
        OSSL_STACK_OF_X509_free(chain_);
        chain_ = nullptr;
        throw std::bad_alloc();
    }
    private_key_ = private_key;
}

tls_certificate::tls_certificate(const tls_certificate& other)
    : tls_certificate(other.certificate_, other.chain_, other.private_key_) {}

tls_certificate::tls_certificate(tls_certificate&& other) noexcept
    : certificate_(std::exchange(other.certificate_, nullptr)),
      chain_(std::exchange(other.chain_, nullptr)),
      private_key_(std::exchange(other.private_key_, nullptr)) {}

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

void tls_certificate::install(SSL_CTX& context) const {
    ::ERR_clear_error();
    if (::SSL_CTX_use_certificate(&context, certificate_) != 1) {
        throw_tls_credential_error("use_certificate");
    }
    ::ERR_clear_error();
    if (::SSL_CTX_use_PrivateKey(&context, private_key_) != 1) {
        throw_tls_credential_error("use_private_key");
    }
    ::ERR_clear_error();
    if (chain_ != nullptr && SSL_CTX_set1_chain(&context, chain_) != 1) {
        throw_tls_credential_error("set1_chain");
    }
    ::ERR_clear_error();
    if (::SSL_CTX_check_private_key(&context) != 1) {
        throw_tls_credential_error("check_private_key");
    }
}

bool tls_certificate::present(SSL& connection) const noexcept {
    // Override the whole slot: an absent chain_ clears any earlier chain.
    return ::SSL_use_cert_and_key(&connection, certificate_, private_key_, chain_, 1) == 1;
}

tls_trust_store tls_trust_store::load_file(const char* file) {
    ::ERR_clear_error();
    std::unique_ptr<X509_STORE, x509_store_deleter> store(X509_STORE_new());
    if (store == nullptr) {
        throw_tls_credential_error("X509_STORE_new");
    }
    if (X509_STORE_load_file(store.get(), file) != 1) {
        throw_tls_credential_error("load_verify_file");
    }
    return tls_trust_store(store.release());
}

tls_trust_store tls_trust_store::load_default() {
    ::ERR_clear_error();
    std::unique_ptr<X509_STORE, x509_store_deleter> store(X509_STORE_new());
    if (store == nullptr) {
        throw_tls_credential_error("X509_STORE_new");
    }
    if (X509_STORE_set_default_paths(store.get()) != 1) {
        throw_tls_credential_error("set_default_verify_paths");
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
