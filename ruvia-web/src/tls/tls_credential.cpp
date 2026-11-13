#include "tls/tls_credential.h"

#include <algorithm>
#include <atomic>
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
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>

#include "ruvia/core/blocking_pool.h"

#include "integration/runtime_control_gate.h"
#include "server/http_server_listener.h"
#include "tls/tls_certificate_store.h"
#include "tls/tls_random.h"

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

namespace ruvia {
namespace {

struct cleanse_identity final {
    tls_identity_config& identity_;
    ~cleanse_identity() {
        for (auto* secret : {&identity_.certificate_chain_pem_, &identity_.private_key_pem_,
                 &identity_.private_key_password_}) {
            if (!secret->empty()) {
                OPENSSL_cleanse(secret->data(), secret->size());
                secret->clear();
            }
        }
    }
};

// Never move a secret-bearing config between coroutine/lambda frames: an SSO
// move may leave bytes in its source. One stable, process-owned payload crosses
// the pool boundary, and every incoming/partially populated copy is cleansed.
struct owned_identity final {
    tls_identity_config value_;
    ~owned_identity() {
        const cleanse_identity cleanse{value_};
    }
};

using owned_identity_pointer = std::unique_ptr<owned_identity>;

void require_accepting_caller(const worker_ref& caller) {
    if (!caller.accepting()) {
        throw std::runtime_error("TLS certificate mutation caller is stopping");
    }
}

}  // namespace

struct tls_certificate_store::state final {
    detail::runtime_control_gate writer_;
    std::atomic<std::shared_ptr<const detail::tls_certificate_snapshot>> snapshot_{
        std::make_shared<const detail::tls_certificate_snapshot>()};

    [[nodiscard]] static task<void> insert_or_assign(
        std::shared_ptr<state> owner, worker_ref caller, tls_certificate_id id,
        owned_identity_pointer identity) {
        const auto lease = co_await owner->writer_.acquire(caller);
        require_accepting_caller(caller);
        auto prepared = co_await run_blocking(caller,
            [current = owner->snapshot_.load(std::memory_order_acquire), id,
                identity = std::move(identity)]() mutable {
                auto certificate = std::make_shared<const detail::tls_certificate>(
                    detail::load_server_tls_certificate(identity->value_));
                identity.reset();
                auto next = std::make_shared<detail::tls_certificate_snapshot>(*current);
                next->insert_or_assign(id, std::move(certificate));
                return next;
            });
        auto next = std::move(prepared).take_value();
        require_accepting_caller(caller);
        owner->snapshot_.store(std::move(next), std::memory_order_release);
    }

    [[nodiscard]] static task<bool> erase(
        std::shared_ptr<state> owner, worker_ref caller, tls_certificate_id id) {
        const auto lease = co_await owner->writer_.acquire(caller);
        require_accepting_caller(caller);
        auto current = owner->snapshot_.load(std::memory_order_acquire);
        if (!current->contains(id)) {
            co_return false;
        }
        // Copying an arbitrarily large immutable map is real preparation, not
        // a synchronous erase disguised by posting a closure to the caller.
        auto prepared = co_await run_blocking(caller,
            [current = std::move(current), id] {
                auto next = std::make_shared<detail::tls_certificate_snapshot>(*current);
                next->erase(id);
                return next;
            });
        auto next = std::move(prepared).take_value();
        require_accepting_caller(caller);
        owner->snapshot_.store(std::move(next), std::memory_order_release);
        co_return true;
    }
};

tls_certificate_store::tls_certificate_store()
    : state_(std::make_shared<state>()) {}
tls_certificate_store::~tls_certificate_store() = default;

task<void> tls_certificate_store::insert_or_assign(
    worker_ref caller, tls_certificate_id id, tls_identity_config identity) {
    const cleanse_identity cleanse{identity};
    auto owner = state_;
    if (id == 0) {
        throw std::invalid_argument("TLS certificate ID zero is reserved");
    }
    auto owned = std::make_unique<owned_identity>();
    owned->value_.certificate_chain_file_ = std::move(identity.certificate_chain_file_);
    owned->value_.private_key_file_ = std::move(identity.private_key_file_);
    owned->value_.certificate_chain_pem_.assign(identity.certificate_chain_pem_);
    owned->value_.private_key_pem_.assign(identity.private_key_pem_);
    owned->value_.private_key_password_.assign(identity.private_key_password_);
    return state::insert_or_assign(std::move(owner), std::move(caller), id, std::move(owned));
}

task<bool> tls_certificate_store::erase(worker_ref caller, tls_certificate_id id) {
    return state::erase(state_, std::move(caller), id);
}

bool tls_certificate_store::contains(tls_certificate_id id) const noexcept {
    return state_->snapshot_.load(std::memory_order_acquire)->contains(id);
}

std::size_t tls_certificate_store::size() const noexcept {
    return state_->snapshot_.load(std::memory_order_acquire)->size();
}

bool tls_certificate_selection::offers_signature_algorithm(std::uint16_t algorithm) const noexcept {
    return std::ranges::find(signature_algorithms_, algorithm) != signature_algorithms_.end();
}

std::shared_ptr<const detail::tls_certificate_snapshot> detail::tls_certificate_store_access::snapshot(
    const tls_certificate_store& store) noexcept {
    return store.state_->snapshot_.load(std::memory_order_acquire);
}

}  // namespace ruvia

namespace ruvia::detail {
namespace {

[[nodiscard]] std::optional<tls_session_ticket_key> generate_ticket_key() noexcept {
    tls_session_ticket_key key;
    if (!fill_random_bytes(key.name_) || !fill_random_bytes(key.hmac_key_) || !fill_random_bytes(key.aes_key_)) {
        return std::nullopt;
    }
    return key;
}

// The ex_data slot that points every TLS context of a listener at its
// worker's key set. Allocated by the first install(), at startup.
[[nodiscard]] int ticket_key_set_index() noexcept {
    static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

[[nodiscard]] bool set_ticket_mac_key(EVP_MAC_CTX& mac, const tls_session_ticket_key& key) noexcept {
    static char digest[] = "SHA256";
    const OSSL_PARAM parameters[] = {
        OSSL_PARAM_construct_octet_string(
            OSSL_MAC_PARAM_KEY, const_cast<unsigned char*>(key.hmac_key_.data()), key.hmac_key_.size()),
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_end(),
    };
    return EVP_MAC_CTX_set_params(&mac, parameters) == 1;
}

[[nodiscard]] bool names_key(const std::optional<tls_session_ticket_key>& key, const unsigned char* name) noexcept {
    return key.has_value() && CRYPTO_memcmp(key->name_.data(), name, key->name_.size()) == 0;
}

}  // namespace

tls_session_ticket_key::~tls_session_ticket_key() {
    OPENSSL_cleanse(name_.data(), name_.size());
    OPENSSL_cleanse(hmac_key_.data(), hmac_key_.size());
    OPENSSL_cleanse(aes_key_.data(), aes_key_.size());
}

tls_session_ticket_key_ring::tls_session_ticket_key_ring(std::chrono::steady_clock::time_point origin)
    : origin_(origin),
      current_(generate_ticket_key()) {
    if (!current_.has_value()) {
        throw std::runtime_error("failed to generate TLS session ticket keys");
    }
}

std::uint64_t tls_session_ticket_key_ring::epoch_at(std::chrono::steady_clock::time_point now) const noexcept {
    if (now <= origin_) {
        return 0;
    }
    return static_cast<std::uint64_t>((now - origin_) / tls_session_lifetime);
}

std::chrono::steady_clock::time_point tls_session_ticket_key_ring::next_rotation(
    std::chrono::steady_clock::time_point now) const noexcept {
    return origin_ + (epoch_at(now) + 1) * std::chrono::duration_cast<std::chrono::steady_clock::duration>(tls_session_lifetime);
}

bool tls_session_ticket_key_ring::copy_to(std::chrono::steady_clock::time_point now, tls_session_ticket_key_set& target) {
    std::optional<tls_session_ticket_key> retired_previous;
    std::optional<tls_session_ticket_key> retired_current;
    std::optional<tls_session_ticket_key> retired_target_current;
    std::optional<tls_session_ticket_key> retired_target_previous;
    bool has_current = false;
    {
        const std::lock_guard lock(mutex_);
        const auto epoch = epoch_at(now);
        if (epoch > epoch_) {
            // The previous key's validity ended with the old epoch; the current
            // key stays valid for decryption through exactly one more epoch.
            retired_previous = std::move(previous_);
            previous_.reset();
            if (epoch == epoch_ + 1) {
                previous_ = current_;
            }
            retired_current = std::move(current_);
            current_.reset();
            epoch_ = epoch;
        }
        if (!current_.has_value()) {
            current_ = generate_ticket_key();
        }
        retired_target_current = std::exchange(target.current_, current_);
        retired_target_previous = std::exchange(target.previous_, previous_);
        has_current = current_.has_value();
    }
    return has_current;
}

tls_session_ticket_key_set::tls_session_ticket_key_set()
    : aes_256_cbc_(EVP_CIPHER_fetch(nullptr, "AES-256-CBC", nullptr)) {
    if (aes_256_cbc_ == nullptr) {
        throw std::runtime_error("failed to fetch the TLS session ticket cipher");
    }
}

tls_session_ticket_key_set::~tls_session_ticket_key_set() {
    EVP_CIPHER_free(aes_256_cbc_);
}

void tls_session_ticket_key_set::install(SSL_CTX& context) {
    const int index = ticket_key_set_index();
    if (index < 0 || SSL_CTX_set_ex_data(&context, index, this) != 1 ||
        SSL_CTX_set_tlsext_ticket_key_evp_cb(&context, &tls_session_ticket_key_set::protect_ticket) != 1) {
        throw std::runtime_error("failed to install TLS session ticket keys");
    }
    (void)SSL_CTX_set_timeout(&context, static_cast<long>(tls_session_lifetime.count()));
}

// SSL_CTX_set_tlsext_ticket_key_evp_cb(3): returns 1 when the key is set up,
// 2 when a ticket decrypted with the previous key should be renewed, 0 when no
// ticket is issued or the key name is unknown (full handshake), -1 on error.
int tls_session_ticket_key_set::protect_ticket(SSL* ssl, unsigned char* key_name, unsigned char* iv,
    EVP_CIPHER_CTX* cipher, EVP_MAC_CTX* mac, int encrypt) noexcept {
    // OpenSSL calls the callback of the connection's initial context, but an
    // SNI switch may already have replaced the context; every context of the
    // listener points at the same set.
    const auto* keys = static_cast<const tls_session_ticket_key_set*>(
        SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), ticket_key_set_index()));
    if (keys == nullptr || cipher == nullptr || mac == nullptr) {
        return -1;
    }
    if (encrypt != 0) {
        if (!keys->current_.has_value()) {
            return 0;
        }
        const auto& key = *keys->current_;
        std::ranges::copy(key.name_, key_name);
        const int iv_size = EVP_CIPHER_get_iv_length(keys->aes_256_cbc_);
        if (iv_size <= 0 || iv_size > EVP_MAX_IV_LENGTH || RAND_bytes(iv, iv_size) != 1 ||
            EVP_EncryptInit_ex2(cipher, keys->aes_256_cbc_, key.aes_key_.data(), iv, nullptr) != 1 ||
            !set_ticket_mac_key(*mac, key)) {
            return -1;
        }
        return 1;
    }
    const bool current = names_key(keys->current_, key_name);
    if (!current && !names_key(keys->previous_, key_name)) {
        return 0;
    }
    const auto& key = current ? *keys->current_ : *keys->previous_;
    if (!set_ticket_mac_key(*mac, key) ||
        EVP_DecryptInit_ex2(cipher, keys->aes_256_cbc_, key.aes_key_.data(), iv, nullptr) != 1) {
        return -1;
    }
    return current ? 1 : 2;
}

}  // namespace ruvia::detail
