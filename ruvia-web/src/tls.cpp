#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <asio/ip/address.hpp>
#include <asio/ssl/context.hpp>
#include <asio/ssl/error.hpp>
#include <asio/ssl/verify_mode.hpp>
#include <asio/system_error.hpp>
#include <openssl/bio.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/ocsp.h>
#include <openssl/params.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "blocking_pool.h"
#include "connection_lifecycle.h"
#include "context_access.h"
#include "http_client_types.h"
#include "http_header.h"
#include "http_request.h"
#include "http_server.h"
#include "server_session.h"
#include "task.h"
#include "worker_ref.h"
#if defined(RUVIA_ENABLE_HTTP3)
#include "http3_connection.h"
#endif
namespace ruvia::detail {
namespace {

using security_callback = int (*)(const SSL*, const SSL_CTX*, int, int, int, void*, void*);

int enforce_security(const SSL*, const SSL_CTX*, int, int, int, void*, void*) noexcept;
void guard_native_handshake(const SSL*, int, int) noexcept;

struct tls_context_policy final {
    tls_context_policy(SSL_CTX& context, std::string_view host, std::uint64_t limit) :
        host_(host),
        record_limit_(limit),
        original_security_(SSL_CTX_get_security_callback(&context)),
        original_security_data_(SSL_CTX_get0_security_ex_data(&context)),
        original_info_(SSL_CTX_get_info_callback(&context)) {
        if (original_security_ == enforce_security) {
            const auto* previous = static_cast<const tls_context_policy*>(original_security_data_);
            original_security_ = previous->original_security_;
            original_security_data_ = previous->original_security_data_;
            if (original_info_ == guard_native_handshake) {
                original_info_ = previous->original_info_;
            }
        }
        if (!is_client_ip_address(host_) && host_.ends_with('.')) {
            host_.pop_back();
        }
    }
    std::string host_;
    std::uint64_t record_limit_;
    security_callback original_security_;
    void* original_security_data_;
    void (*original_info_)(const SSL*, int, int);
};

int enforce_security(const SSL* ssl, const SSL_CTX* context, int operation, int bits, int nid, void* other, void* opaque) noexcept {
    const auto& policy = *static_cast<const tls_context_policy*>(opaque);
    if (operation == SSL_SECOP_VERSION) {
        if (nid < TLS1_1_VERSION) {
            return 0;
        }
        if (nid == TLS1_1_VERSION) {
            return 1;
        }
    }
    if (operation == SSL_SECOP_COMPRESSION) {
        return 0;
    }
    if (operation == SSL_SECOP_CIPHER_SUPPORTED || operation == SSL_SECOP_CIPHER_SHARED || operation == SSL_SECOP_CIPHER_CHECK) {
        const auto* cipher = static_cast<const SSL_CIPHER*>(other);
        switch (SSL_CIPHER_get_id(cipher)) {
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256:
            case TLS1_CK_ECDHE_RSA_WITH_AES_128_GCM_SHA256:
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384:
            case TLS1_CK_ECDHE_RSA_WITH_AES_256_GCM_SHA384:
            case TLS1_CK_ECDHE_ECDSA_WITH_CHACHA20_POLY1305:
            case TLS1_CK_ECDHE_RSA_WITH_CHACHA20_POLY1305:
                break;
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_128_CBC_SHA:
            case TLS1_CK_ECDHE_RSA_WITH_AES_128_CBC_SHA:
            case TLS1_CK_ECDHE_ECDSA_WITH_AES_256_CBC_SHA:
            case TLS1_CK_ECDHE_RSA_WITH_AES_256_CBC_SHA:
                // SUPPORTED runs before the peer's version is known. Advertise
                // CBC for fallback, but never select or accept it with TLS 1.2.
                if (operation != SSL_SECOP_CIPHER_SUPPORTED && (!ssl || SSL_version(ssl) != TLS1_1_VERSION)) {
                    return 0;
                }
                break;
            default:
                if (std::string_view(SSL_CIPHER_get_version(cipher)) != "TLSv1.3") {
                    return 0;
                }
        }
    }
    if (ssl && SSL_version(ssl) == TLS1_1_VERSION && (nid == NID_md5_sha1 || nid == NID_sha1) && (operation == SSL_SECOP_SIGALG_SUPPORTED || operation == SSL_SECOP_SIGALG_SHARED || operation == SSL_SECOP_SIGALG_CHECK || operation == SSL_SECOP_SIGALG_MASK)) {
        // These are handshake signature operations only. In particular,
        // SSL_SECOP_CA_MD, EE_KEY and CA_KEY are never exempted.
        return 1;
    }
    return policy.original_security_ && policy.original_security_(ssl, context, operation, bits, nid, other, policy.original_security_data_);
}

int deny_security(const SSL*, const SSL_CTX*, int, int, int, void*, void*) noexcept {
    return 0;
}

struct tls_record_state final {
    std::uint64_t limit_;
    std::uint64_t records_{0};
    bool blocked_{false};
};

void free_context_policy(void*, void* value, CRYPTO_EX_DATA*, int, long, void*) noexcept {
    delete static_cast<tls_context_policy*>(value);
}

void free_record_state(void*, void* value, CRYPTO_EX_DATA*, int, long, void*) noexcept {
    delete static_cast<tls_record_state*>(value);
}

int copy_record_state(CRYPTO_EX_DATA*, const CRYPTO_EX_DATA*, void** value, int, long, void*) noexcept {
    try {
        if (*value) {
            *value = new tls_record_state(*static_cast<tls_record_state*>(*value));
        }
        return 1;
    } catch (...) {
        return 0;
    }
}

int context_index = -1;
int record_index = -1;
int bio_index = -1;

void guard_native_handshake(const SSL* ssl, int where, int result) noexcept {
    if ((where & SSL_CB_HANDSHAKE_START) != 0) {
        // Unlike SSL_new's ex_data notification, this runs only after the
        // connection is initialized and before any handshake is processed.
        SSL_set_options(const_cast<SSL*>(ssl), SSL_OP_NO_RENEGOTIATION | SSL_OP_NO_COMPRESSION);
    }
    const auto* policy = static_cast<const tls_context_policy*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), context_index));
    if (policy && policy->original_info_) {
        policy->original_info_(ssl, where, result);
    }
}

long deny_output(BIO* bio, int operation, const char*, std::size_t, int, long, int result, std::size_t* processed) noexcept {
    if (operation == BIO_CB_WRITE) {
        ERR_raise_data(ERR_LIB_SSL, ERR_R_OPERATION_FAIL, "TLS outgoing record limit or policy allocation failure");
        BIO_clear_retry_flags(bio);
        if (processed) {
            *processed = 0;
        }
        return 0;
    }
    return result;
}

long guard_output(BIO* bio, int operation, const char* bytes, std::size_t size, int arg, long larg, int result, std::size_t* processed) noexcept {
    if (operation == BIO_CB_WRITE) {
        const auto* state = static_cast<const tls_record_state*>(BIO_get_ex_data(bio, bio_index));
        if (!state || state->blocked_) {
            return deny_output(bio, operation, bytes, size, arg, larg, result, processed);
        }
    }
    return result;
}

void block_messages(int writing, int, int, const void*, std::size_t, SSL* ssl, void*) noexcept {
    if (writing) {
        if (auto* bio = SSL_get_wbio(ssl)) {
            BIO_set_callback_ex(bio, deny_output);
        }
    }
}

}  // namespace

void count_tls_records(int writing, int, int content_type, const void*, std::size_t, SSL* ssl, void*) noexcept {
    if (!writing) {
        return;
    }
    auto* bio = SSL_get_wbio(ssl);
    if (!bio) {
        return;
    }
    try {
        auto* state = static_cast<tls_record_state*>(SSL_get_ex_data(ssl, record_index));
        if (!state) {
            const auto* policy = static_cast<const tls_context_policy*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), context_index));
            if (!policy) {
                BIO_set_callback_ex(bio, deny_output);
                return;
            }
            auto owner = std::make_unique<tls_record_state>(tls_record_state{policy->record_limit_});
            if (SSL_set_ex_data(ssl, record_index, owner.get()) != 1) {
                throw std::bad_alloc();
            }
            state = owner.release();
        }
        if (content_type == SSL3_RT_HEADER) {
            if (state->records_ == state->limit_) {
                state->blocked_ = true;
            } else {
                ++state->records_;
            }
        }
        // CONNECT 会替换写 BIO；所有密文生产者仍绑定同一连接级计数。
        if (BIO_get_ex_data(bio, bio_index) != state && BIO_set_ex_data(bio, bio_index, state) != 1) {
            throw std::bad_alloc();
        }
        BIO_set_callback_ex(bio, guard_output);
    } catch (...) {
        ERR_raise(ERR_LIB_SSL, ERR_R_MALLOC_FAILURE);
        BIO_set_callback_ex(bio, deny_output);
    }
}

namespace {

int accept_verified_chain(int valid, X509_STORE_CTX*) noexcept {
    return valid;
}

int deny_certificate(X509_STORE_CTX*, void*) noexcept {
    return 0;
}

int verify_native_certificate(X509_STORE_CTX* store, void* opaque) noexcept {
    const auto& policy = *static_cast<const tls_context_policy*>(opaque);
    // MariaDB 的默认回调会延迟并容忍部分错误；这里在握手内完成严格校验。
    X509_STORE_CTX_set_verify_cb(store, accept_verified_chain);
    if (X509_verify_cert(store) != 1) {
        return 0;
    }
    auto* leaf = X509_STORE_CTX_get0_cert(store);
    if (!leaf) {
        X509_STORE_CTX_set_error(store, X509_V_ERR_APPLICATION_VERIFICATION);
        return 0;
    }
    const bool ip = is_client_ip_address(policy.host_);
    const auto matched = ip ? X509_check_ip_asc(leaf, policy.host_.c_str(), 0) : X509_check_host(leaf, policy.host_.c_str(), policy.host_.size(), X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr);
    if (matched == 1) {
        return 1;
    }
    X509_STORE_CTX_set_error(store, matched == 0 ? (ip ? X509_V_ERR_IP_ADDRESS_MISMATCH : X509_V_ERR_HOSTNAME_MISMATCH) : X509_V_ERR_APPLICATION_VERIFICATION);
    return 0;
}

void install_context_callbacks(SSL_CTX& context, tls_context_policy* policy) noexcept {
    SSL_CTX_set0_security_ex_data(&context, policy);
    SSL_CTX_set_security_callback(&context, enforce_security);
    SSL_CTX_set_msg_callback(&context, count_tls_records);
    SSL_CTX_set_msg_callback_arg(&context, nullptr);
    if (!policy->host_.empty()) {
        SSL_CTX_set_cert_verify_callback(&context, verify_native_certificate, policy);
        SSL_CTX_set_info_callback(&context, guard_native_handshake);
    }
}

}  // namespace

thread_local native_tls_context_scope* native_tls_context_scope::current_ = nullptr;

// Registers, once per process, the ex_data slots that carry the policy on
// every context, connection and BIO.
void register_tls_policy_indexes() {
    static const bool registered = [] {
        context_index = SSL_CTX_get_ex_new_index(0, nullptr, native_tls_context_scope::new_context, nullptr, free_context_policy);
        record_index = SSL_get_ex_new_index(0, nullptr, nullptr, copy_record_state, free_record_state);
        bio_index = BIO_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
        if (context_index < 0 || record_index < 0 || bio_index < 0) {
            throw std::runtime_error("failed to register TLS policy storage");
        }
        return true;
    }();
    (void)registered;
}

native_tls_context_scope::native_tls_context_scope(std::string_view host, std::uint64_t record_limit) :
    host_(host),
    record_limit_(record_limit),
    previous_(current_) {
    // 构造通知只安装回调和 ex_data；不查询未初始化的 context 参数或 SSL 状态。
    register_tls_policy_indexes();
    current_ = host_.empty() ? nullptr : this;
}

native_tls_context_scope::~native_tls_context_scope() {
    current_ = previous_;
}

void native_tls_context_scope::rethrow_failure() const {
    if (failure_) {
        std::rethrow_exception(failure_);
    }
}

void native_tls_context_scope::new_context(void* parent, void*, CRYPTO_EX_DATA* storage, int index, long, void*) noexcept {
    if (!current_) {
        return;
    }
    auto* context = static_cast<SSL_CTX*>(parent);
    try {
        auto owner = std::make_unique<tls_context_policy>(*context, current_->host_, current_->record_limit_);
        if (CRYPTO_set_ex_data(storage, index, owner.get()) != 1) {
            throw std::bad_alloc();
        }
        install_context_callbacks(*context, owner.release());
    } catch (...) {
        current_->failure_ = std::current_exception();
        SSL_CTX_set_security_callback(context, deny_security);
        SSL_CTX_set_cert_verify_callback(context, deny_certificate, nullptr);
        SSL_CTX_set_msg_callback(context, block_messages);
    }
}

void validate_tls_record_limit(std::uint64_t limit) {
    if (limit == 0 || limit > (std::uint64_t{1} << 23)) {
        throw std::invalid_argument("TLS outgoing record limit must be in 1..2^23");
    }
}

void apply_tls_protocol_policy(SSL_CTX& context, std::uint64_t record_limit) {
    validate_tls_record_limit(record_limit);
    register_tls_policy_indexes();
    auto owner = std::make_unique<tls_context_policy>(context, std::string_view{}, record_limit);
    auto* previous = static_cast<tls_context_policy*>(SSL_CTX_get_ex_data(&context, context_index));
    if (SSL_CTX_set_ex_data(&context, context_index, owner.get()) != 1) {
        throw std::bad_alloc();
    }
    auto* policy = owner.release();
    delete previous;
    install_context_callbacks(context, policy);
    if (SSL_CTX_set_min_proto_version(&context, TLS1_1_VERSION) != 1 || SSL_CTX_set_max_proto_version(&context, 0) != 1) {
        throw std::runtime_error("failed to configure TLS versions");
    }
    SSL_CTX_set_options(&context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    if (SSL_CTX_set_cipher_list(&context, tls_cipher_suites) != 1) {
        throw std::runtime_error("failed to configure TLS cipher suites");
    }
}

namespace {

[[nodiscard]] std::string_view origin_key(const http_target_origin& origin) noexcept {
    if (origin.host_kind() == http_target_host_kind::dns_name) {
        return origin.host();
    }
    const auto address = origin.address();
    return {reinterpret_cast<const char*>(address.data()), address.size()};
}

}  // namespace

bool tls_served_certificate::covers(const http_target_origin& origin) noexcept {
    if (origin.action() != http_target_origin_action::verify_certificate) {
        return false;
    }
    const auto key = origin_key(origin);
    if (key.empty()) {
        return false;
    }
    const bool dns_name = origin.host_kind() == http_target_host_kind::dns_name;
    for (std::size_t i = 0; i < cached_; ++i) {
        const auto& entry = cache_[i];
        if (entry.kind_ != origin.host_kind() || entry.key_size_ != key.size()) {
            continue;
        }
        const std::string_view cached_key(key_storage_.data() + entry.key_offset_, entry.key_size_);
        if (dns_name ? http_ascii_equals_ignore_case(cached_key, key) : cached_key == key) {
            return entry.covered_;
        }
    }
    const bool covered = check(origin);
    if (key.size() <= cached_key_capacity) {
        if (cached_ == cache_size || key.size() > key_storage_capacity - key_used_) {
            cached_ = 0;
            key_used_ = 0;
        }
        auto& entry = cache_[cached_++];
        entry.key_offset_ = static_cast<std::uint8_t>(key_used_);
        entry.key_size_ = static_cast<std::uint8_t>(key.size());
        entry.kind_ = origin.host_kind();
        entry.covered_ = covered;
        std::copy_n(key.data(), key.size(), key_storage_.data() + key_used_);
        key_used_ = static_cast<std::uint16_t>(key_used_ + key.size());
    }
    return covered;
}

bool tls_served_certificate::check(const http_target_origin& origin) const noexcept {
    if (certificate_ == nullptr) {
        return false;
    }
    if (origin.host_kind() == http_target_host_kind::dns_name) {
        const auto host = origin.host();
        return X509_check_host(certificate_, host.data(), host.size(), X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT, nullptr) == 1;
    }
    const auto address = origin.address();
    return X509_check_ip(certificate_, address.data(), address.size(), 0) == 1;
}

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
    const std::unique_ptr<X509, x509_deleter> leaf(PEM_read_bio_X509_AUX(input.get(), nullptr, password_callback, password_userdata));
    if (leaf == nullptr || SSL_CTX_use_certificate(&context, leaf.get()) != 1) {
        throw_tls_credential_error("use_certificate_chain_pem");
    }
    for (;;) {
        std::unique_ptr<X509, x509_deleter> issuer(PEM_read_bio_X509(input.get(), nullptr, password_callback, password_userdata));
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
    const std::unique_ptr<EVP_PKEY, private_key_deleter> private_key(PEM_read_bio_PrivateKey(input.get(), nullptr, SSL_CTX_get_default_passwd_cb(&context), SSL_CTX_get_default_passwd_cb_userdata(&context)));
    if (private_key == nullptr || SSL_CTX_use_PrivateKey(&context, private_key.get()) != 1) {
        throw_tls_credential_error("use_private_key_pem");
    }
}

// Borrow the configured password only during synchronous credential loading.
// An absent password uses a caller provider, or an empty noninteractive default.
class tls_password_scope final {
public:
    tls_password_scope(SSL_CTX& context_value, std::string_view password) noexcept :
        context_(&context_value),
        password_(password),
        previous_callback_(SSL_CTX_get_default_passwd_cb(&context_value)),
        previous_userdata_(SSL_CTX_get_default_passwd_cb_userdata(&context_value)) {
        const bool default_source = previous_userdata_ == nullptr && (previous_callback_ == nullptr || previous_callback_ == PEM_def_callback);
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
    const auto error = ERR_SYSTEM_ERROR(code) ? std::error_code(ERR_GET_REASON(code), asio::error::get_system_category()) : std::error_code(static_cast<int>(code), asio::error::get_ssl_category());
    throw asio::system_error(error, operation);
}

tls_certificate tls_certificate::load(const char* certificate_chain_file, const char* private_key_file, std::string_view private_key_password) {
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

tls_certificate tls_certificate::load_pem(std::string_view certificate_chain, std::string_view private_key, std::string_view private_key_password) {
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

tls_certificate::tls_certificate(const tls_certificate& other) :
    tls_certificate(other.certificate_, other.chain_, other.private_key_) {}

tls_certificate::tls_certificate(tls_certificate&& other) noexcept :
    certificate_(std::exchange(other.certificate_, nullptr)),
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

tls_trust_store::tls_trust_store(const tls_trust_store& other) :
    store_(other.store_) {
    if (store_ != nullptr && X509_STORE_up_ref(store_) != 1) {
        store_ = nullptr;
        throw std::bad_alloc();
    }
}

tls_trust_store::tls_trust_store(tls_trust_store&& other) noexcept :
    store_(std::exchange(other.store_, nullptr)) {}

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
        for (auto* secret : {&identity_.certificate_chain_pem_, &identity_.private_key_pem_, &identity_.private_key_password_}) {
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
    std::atomic<std::shared_ptr<const detail::tls_certificate_snapshot>> snapshot_{std::make_shared<const detail::tls_certificate_snapshot>()};

    [[nodiscard]] static task<void> insert_or_assign(std::shared_ptr<state> owner, worker_ref caller, tls_certificate_id id, owned_identity_pointer identity) {
        const auto lease = co_await owner->writer_.acquire(caller);
        require_accepting_caller(caller);
        auto prepared = co_await run_blocking(caller, [current = owner->snapshot_.load(std::memory_order_acquire), id, identity = std::move(identity)]() mutable {
            auto certificate = std::make_shared<const detail::tls_certificate>(detail::load_server_tls_certificate(identity->value_));
            identity.reset();
            auto next = std::make_shared<detail::tls_certificate_snapshot>(*current);
            next->insert_or_assign(id, std::move(certificate));
            return next;
        });
        auto next = std::move(prepared).take_value();
        require_accepting_caller(caller);
        owner->snapshot_.store(std::move(next), std::memory_order_release);
    }

    [[nodiscard]] static task<bool> erase(std::shared_ptr<state> owner, worker_ref caller, tls_certificate_id id) {
        const auto lease = co_await owner->writer_.acquire(caller);
        require_accepting_caller(caller);
        auto current = owner->snapshot_.load(std::memory_order_acquire);
        if (!current->contains(id)) {
            co_return false;
        }
        // Copying an arbitrarily large immutable map is real preparation, not
        // a synchronous erase disguised by posting a closure to the caller.
        auto prepared = co_await run_blocking(caller, [current = std::move(current), id] {
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

tls_certificate_store::tls_certificate_store() :
    state_(std::make_shared<state>()) {}
tls_certificate_store::~tls_certificate_store() = default;

task<void> tls_certificate_store::insert_or_assign(worker_ref caller, tls_certificate_id id, tls_identity_config identity) {
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

std::shared_ptr<const detail::tls_certificate_snapshot> detail::tls_certificate_store_access::snapshot(const tls_certificate_store& store) noexcept {
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
        OSSL_PARAM_construct_octet_string(OSSL_MAC_PARAM_KEY, const_cast<unsigned char*>(key.hmac_key_.data()), key.hmac_key_.size()),
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

tls_session_ticket_key_ring::tls_session_ticket_key_ring(std::chrono::steady_clock::time_point origin) :
    origin_(origin),
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

std::chrono::steady_clock::time_point tls_session_ticket_key_ring::next_rotation(std::chrono::steady_clock::time_point now) const noexcept {
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

tls_session_ticket_key_set::tls_session_ticket_key_set() :
    aes_256_cbc_(EVP_CIPHER_fetch(nullptr, "AES-256-CBC", nullptr)) {
    if (aes_256_cbc_ == nullptr) {
        throw std::runtime_error("failed to fetch the TLS session ticket cipher");
    }
}

tls_session_ticket_key_set::~tls_session_ticket_key_set() {
    EVP_CIPHER_free(aes_256_cbc_);
}

void tls_session_ticket_key_set::install(SSL_CTX& context) {
    const int index = ticket_key_set_index();
    if (index < 0 || SSL_CTX_set_ex_data(&context, index, this) != 1 || SSL_CTX_set_tlsext_ticket_key_evp_cb(&context, &tls_session_ticket_key_set::protect_ticket) != 1) {
        throw std::runtime_error("failed to install TLS session ticket keys");
    }
    (void)SSL_CTX_set_timeout(&context, static_cast<long>(tls_session_lifetime.count()));
}

// SSL_CTX_set_tlsext_ticket_key_evp_cb(3): returns 1 when the key is set up,
// 2 when a ticket decrypted with the previous key should be renewed, 0 when no
// ticket is issued or the key name is unknown (full handshake), -1 on error.
int tls_session_ticket_key_set::protect_ticket(SSL* ssl, unsigned char* key_name, unsigned char* iv, EVP_CIPHER_CTX* cipher, EVP_MAC_CTX* mac, int encrypt) noexcept {
    // OpenSSL calls the callback of the connection's initial context, but an
    // SNI switch may already have replaced the context; every context of the
    // listener points at the same set.
    const auto* keys = static_cast<const tls_session_ticket_key_set*>(SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), ticket_key_set_index()));
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
        if (iv_size <= 0 || iv_size > EVP_MAX_IV_LENGTH || RAND_bytes(iv, iv_size) != 1 || EVP_EncryptInit_ex2(cipher, keys->aes_256_cbc_, key.aes_key_.data(), iv, nullptr) != 1 || !set_ticket_mac_key(*mac, key)) {
            return -1;
        }
        return 1;
    }
    const bool current = names_key(keys->current_, key_name);
    if (!current && !names_key(keys->previous_, key_name)) {
        return 0;
    }
    const auto& key = current ? *keys->current_ : *keys->previous_;
    if (!set_ticket_mac_key(*mac, key) || EVP_DecryptInit_ex2(cipher, keys->aes_256_cbc_, key.aes_key_.data(), iv, nullptr) != 1) {
        return -1;
    }
    return current ? 1 : 2;
}

namespace {

int quic_resumption_context_index() noexcept {
    static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
    return index;
}

int generate_quic_ticket(SSL* ssl, void*) noexcept {
    const auto* context = static_cast<const std::array<unsigned char, 32>*>(SSL_get_ex_data(ssl, quic_resumption_context_index()));
    return context != nullptr && SSL_SESSION_set1_ticket_appdata(SSL_get0_session(ssl), context->data(), context->size()) == 1;
}

SSL_TICKET_RETURN decrypt_quic_ticket(SSL* ssl, SSL_SESSION* ticket, const unsigned char*, std::size_t, SSL_TICKET_STATUS status, void*) noexcept {
    if (status != SSL_TICKET_SUCCESS && status != SSL_TICKET_SUCCESS_RENEW) {
        return SSL_TICKET_RETURN_IGNORE_RENEW;
    }
    const auto* context = static_cast<const std::array<unsigned char, 32>*>(SSL_get_ex_data(ssl, quic_resumption_context_index()));
    void* remembered = nullptr;
    std::size_t remembered_size = 0;
    // RFC 9001 §4.6.1 and RFC 9114 §7.2.8: changed transport limits or
    // SETTINGS require a fresh handshake, not acceptance of stale 0-RTT.
    if (context == nullptr || SSL_SESSION_get0_ticket_appdata(ticket, &remembered, &remembered_size) != 1 || !constant_time_bytes_equal(std::string_view(static_cast<const char*>(remembered), remembered_size), std::string_view(reinterpret_cast<const char*>(context->data()), context->size()))) {
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

bool select_handshake_certificate(SSL* ssl, http_server_session_config& session, std::string_view server_name, int* alert) noexcept;

// OpenSSL reports each inbound handshake message once it is reassembled and
// before it processes it, so a ClientHello reaches the observer before the
// client hello, server_name and certificate callbacks run.
void deliver_client_hello(int writing, int content_type, const void* bytes, std::size_t size, SSL* ssl) noexcept {
    // msg_type and the 3-byte length (RFC 9846 §4) precede every body.
    if (writing != 0 || content_type != SSL3_RT_HANDSHAKE || size < 4 || *static_cast<const unsigned char*>(bytes) != SSL3_MT_CLIENT_HELLO) {
        return;
    }
    const auto* observation = static_cast<const tls_client_hello_observation*>(SSL_get_ex_data(ssl, client_hello_observation_index()));
    if (observation == nullptr || !observation->callback_) {
        return;
    }
    auto fields = observation->fields_;
    fields.message_ = std::span(static_cast<const std::byte*>(bytes), size);
    observation->callback_(server_observation_access::tls_client_hello(fields));
}

// TLS over TCP keeps the protocol policy's outgoing record count.
void observe_tcp_messages(int writing, int version, int content_type, const void* bytes, std::size_t size, SSL* ssl, void* argument) noexcept {
    count_tls_records(writing, version, content_type, bytes, size, ssl, argument);
    deliver_client_hello(writing, content_type, bytes, size, ssl);
}

// QUIC protects its own packets (RFC 9001 §4): there are no TLS records to count.
void observe_quic_messages(int writing, int, int content_type, const void* bytes, std::size_t size, SSL* ssl, void*) noexcept {
    deliver_client_hello(writing, content_type, bytes, size, ssl);
}

int select_tcp_alpn(SSL* ssl, std::span<const unsigned char> protocols, const unsigned char** out, unsigned char* out_length, const unsigned char* in, unsigned int in_length) noexcept {
    if (SSL_version(ssl) >= TLS1_2_VERSION) {
        return SSL_select_next_proto(const_cast<unsigned char**>(out), out_length, protocols.data(), static_cast<unsigned int>(protocols.size()), in, in_length) == OPENSSL_NPN_NEGOTIATED ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_ALERT_FATAL;
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
        if (SSL_select_next_proto(const_cast<unsigned char**>(out), out_length, protocol, static_cast<unsigned int>(length) + 1, in, in_length) == OPENSSL_NPN_NEGOTIATED) {
            return SSL_TLSEXT_ERR_OK;
        }
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

// Every HTTP version this server speaks over TLS on TCP, in preference order;
// RFC 7301 §3.2 reserves the alert for offers naming none of them.
int select_tcp_alpn_protocol(SSL* ssl, const unsigned char** out, unsigned char* out_length, const unsigned char* in, unsigned int in_length, void*) noexcept {
    static constexpr unsigned char protocols[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1', 8, 'h', 't', 't', 'p', '/', '1', '.', '0'};
    return select_tcp_alpn(ssl, protocols, out, out_length, in, in_length);
}

// RFC 9114 §3.1: an HTTP/3 connection negotiates "h3", and RFC 9001 §8.1
// requires the handshake to fail when ALPN agrees on nothing.
int select_quic_alpn_protocol(SSL*, const unsigned char** out, unsigned char* out_length, const unsigned char* in, unsigned int in_length, void*) noexcept {
    static constexpr unsigned char protocols[] = {2, 'h', '3'};
    if (SSL_select_next_proto(const_cast<unsigned char**>(out), out_length, protocols, static_cast<unsigned int>(sizeof(protocols)), in, in_length) == OPENSSL_NPN_NEGOTIATED) {
        return SSL_TLSEXT_ERR_OK;
    }
    return SSL_TLSEXT_ERR_ALERT_FATAL;
}

// A connection-dispatch listener offers its own protocols (tls_config::alpn_),
// still in server preference and failing an offer that names none of them.
int select_connection_alpn_protocol(SSL* ssl, const unsigned char** out, unsigned char* out_length, const unsigned char* in, unsigned int in_length, void* argument) noexcept {
    const auto& protocols = *static_cast<const std::string*>(argument);
    return select_tcp_alpn(ssl, {reinterpret_cast<const unsigned char*>(protocols.data()), protocols.size()}, out, out_length, in, in_length);
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
        if (extension_size < 5 || static_cast<std::size_t>((extension[0] << 8) | extension[1]) != extension_size - 2 || extension[2] != TLSEXT_NAMETYPE_host_name) {
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
            if (EVP_Digest(name.data(), name_size, context.data(), &digest_size, EVP_sha256(), nullptr) != 1 || digest_size != context.size()) {
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
    const auto length = prefix == 1 ? static_cast<std::size_t>(bytes[0]) : static_cast<std::size_t>((bytes[0] << 8) | bytes[1]);
    if (length != size - prefix) {
        throw std::invalid_argument("malformed TLS selection extension length");
    }
    return {reinterpret_cast<const std::byte*>(bytes + prefix), length};
}

std::vector<std::uint16_t> decode_u16_list(std::span<const std::byte> raw) {
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

std::vector<std::string_view> decode_alpn_list(std::span<const std::byte> raw) {
    std::vector<std::string_view> protocols;
    for (std::size_t pos = 0; pos < raw.size();) {
        const auto length = static_cast<std::size_t>(std::to_integer<unsigned int>(raw[pos]));
        if (length == 0U || pos + 1U + length > raw.size()) {
            throw std::invalid_argument("malformed TLS ALPN extension list");
        }
        protocols.emplace_back(reinterpret_cast<const char*>(raw.data() + pos + 1U), length);
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
    if (identity.ocsp_response_.empty() || SSL_client_hello_get0_ext(&ssl, TLSEXT_TYPE_status_request, &extension, &size) != 1 || size == 0 || extension[0] != TLSEXT_STATUSTYPE_ocsp) {
        return true;
    }
    auto* response = static_cast<unsigned char*>(OPENSSL_memdup(identity.ocsp_response_.data(), identity.ocsp_response_.size()));
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
    return SSL_get_tlsext_status_ocsp_resp(ssl, &response) > 0 ? SSL_TLSEXT_ERR_OK : SSL_TLSEXT_ERR_NOACK;
}

// Runs before session lookup, so neither resumption nor missing SNI bypasses a
// selector rejection. A HelloRetryRequest retains the first selected identity.
bool select_handshake_certificate(SSL* ssl, http_server_session_config& session, std::string_view server_name, int* alert) noexcept {
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
                const auto* binding = static_cast<const tls_client_hello_observation*>(SSL_get_ex_data(ssl, client_hello_observation_index()));
                if (!binding || binding->fields_.connection_id_ == 0 || local == nullptr) {
                    throw std::logic_error("TLS certificate selector requires connection metadata");
                }
                const unsigned char* ciphers = nullptr;
                const auto cipher_size = SSL_client_hello_get0_ciphers(ssl, &ciphers);
                const auto cipher_suites = decode_u16_list({reinterpret_cast<const std::byte*>(ciphers), cipher_size});
                const auto signature_algorithms = decode_u16_list(client_hello_list(ssl, TLSEXT_TYPE_signature_algorithms, 2));
                const auto supported_versions = decode_u16_list(client_hello_list(ssl, TLSEXT_TYPE_supported_versions, 1));
                const auto supported_groups = decode_u16_list(client_hello_list(ssl, TLSEXT_TYPE_supported_groups, 2));
                const auto alpn = decode_alpn_list(client_hello_list(ssl, TLSEXT_TYPE_application_layer_protocol_negotiation, 2));
                const tls_certificate_selection input{.connection_id_ = binding->fields_.connection_id_, .remote_address_ = ip_address::parse(binding->fields_.remote_address_).value_or(ip_address{}), .remote_port_ = binding->fields_.remote_port_, .server_name_ = server_name, .local_address_ = local ? std::optional(to_ip_address(*local)) : std::nullopt, .local_port_ = binding->fields_.local_port_, .quic_ = binding->fields_.quic_, .legacy_version_ = static_cast<std::uint16_t>(SSL_client_hello_get0_legacy_version(ssl)), .cipher_suites_ = cipher_suites, .signature_algorithms_ = signature_algorithms, .supported_versions_ = supported_versions, .supported_groups_ = supported_groups, .alpn_ = alpn};
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
        if (SSL_CTX_set_min_proto_version(native, TLS1_3_VERSION) != 1 || SSL_CTX_set_max_proto_version(native, TLS1_3_VERSION) != 1) {
            throw std::runtime_error("failed to restrict the QUIC TLS context to TLS 1.3");
        }
        SSL_CTX_set_alpn_select_cb(native, select_quic_alpn_protocol, nullptr);
        if (SSL_CTX_set_max_early_data(native, early_data ? UINT32_MAX : 0) != 1 || SSL_CTX_set_session_ticket_cb(native, generate_quic_ticket, decrypt_quic_ticket, nullptr) != 1) {
            throw std::runtime_error("failed to configure QUIC session tickets");
        }
        if (early_data) {
            // Ticket keys are shared across workers, not OpenSSL's local
            // replay cache. RFC 8470 admission rejects unsafe early requests.
            SSL_CTX_set_options(native, SSL_OP_NO_ANTI_REPLAY);
        }
    } else {
        context_value.set_options(asio::ssl::context::default_workarounds | asio::ssl::context::no_sslv2 | asio::ssl::context::no_sslv3 | asio::ssl::context::no_tlsv1 | asio::ssl::context::single_dh_use);
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
void configure_http3_server_tls_session(SSL& tls, const http3_quic_connection& connection, std::uint32_t version, const http3_connection_options& options, bool early_data, std::array<unsigned char, 32>& resumption_context) {
    // Only remembered limits (RFC 9000 §7.4.1): connection IDs and reset
    // tokens change for each connection and must not prevent legitimate
    // ticket resumption.
    std::array<std::byte, 256> parameters{};
    const auto parameter_bytes = connection.encode_early_transport_parameters(parameters);
    if (!parameter_bytes.has_value()) {
        throw std::runtime_error("failed to encode QUIC early transport parameters");
    }
    const std::array<std::uint64_t, 7> values{version, options.max_field_section_size_, options.qpack_max_dtable_capacity_, options.qpack_blocked_streams_, static_cast<std::uint64_t>(options.enable_connect_protocol_), static_cast<std::uint64_t>(options.enable_h3_datagram_), static_cast<std::uint64_t>(early_data)};
    std::array<unsigned char, values.size() * sizeof(std::uint64_t) + parameters.size()> encoded{};
    for (std::size_t i = 0; i < values.size(); ++i) {
        for (std::size_t byte = 0; byte < sizeof(std::uint64_t); ++byte) {
            encoded[i * sizeof(std::uint64_t) + byte] = static_cast<unsigned char>(values[i] >> (8 * (sizeof(std::uint64_t) - byte - 1)));
        }
    }
    const auto encoded_size = values.size() * sizeof(std::uint64_t) + *parameter_bytes;
    std::memcpy(encoded.data() + values.size() * sizeof(std::uint64_t), parameters.data(), *parameter_bytes);
    unsigned int digest_size = 0;
    const auto index = quic_resumption_context_index();
    if (index < 0 || EVP_Digest(encoded.data(), encoded_size, resumption_context.data(), &digest_size, EVP_sha256(), nullptr) != 1 || digest_size != resumption_context.size() || SSL_set_ex_data(&tls, index, &resumption_context) != 1 || SSL_set_quic_tls_early_data_enabled(&tls, early_data ? 1 : 0) != 1) {
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

prepared_http_server_session prepare_http_server_session_config(const http_server_listener_definition& definition, bool observe_client_hello) {
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
    const std::unique_ptr<OCSP_RESPONSE, decltype(&OCSP_RESPONSE_free)> response(d2i_OCSP_RESPONSE(nullptr, &cursor, static_cast<long>(bytes.size())), OCSP_RESPONSE_free);
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
    if (i2d_OCSP_RESPONSE(response.get(), &output) != size || !std::equal(bytes.begin(), bytes.end(), canonical.begin())) {
        throw std::invalid_argument("TLS OCSP response must be a DER OCSPResponse");
    }
}

// One identity's validated source and the password its key needs.
struct tls_identity_source final {
    std::variant<tls_identity_files, tls_identity_pem> location_;
    std::string_view private_key_password_;
    std::span<const std::byte> ocsp_response_;
};

[[nodiscard]] tls_identity_source make_tls_identity_source(const tls_identity_config& identity, std::span<const std::byte> ocsp_response = {}) {
    validate_ocsp_response(ocsp_response);
    const bool files = !identity.certificate_chain_file_.empty() || !identity.private_key_file_.empty();
    const bool pem = !identity.certificate_chain_pem_.empty() || !identity.private_key_pem_.empty();
    if (files && pem) {
        throw std::invalid_argument("TLS identity must come from PEM files or in-memory PEM, not both");
    }
    if (!pem) {
        // Both names are required: an empty one throws.
        return {.location_ = make_tls_identity_files(identity), .private_key_password_ = identity.private_key_password_, .ocsp_response_ = ocsp_response};
    }
    if (identity.certificate_chain_pem_.empty() || identity.private_key_pem_.empty()) {
        throw std::invalid_argument("in-memory TLS identity requires both certificate chain and private key PEM");
    }
    return {.location_ = tls_identity_pem{.certificate_chain_ = identity.certificate_chain_pem_, .private_key_ = identity.private_key_pem_}, .private_key_password_ = identity.private_key_password_, .ocsp_response_ = ocsp_response};
}

[[nodiscard]] tls_certificate load_tls_identity(const tls_identity_source& source) {
    if (const auto* files = std::get_if<tls_identity_files>(&source.location_)) {
        return load_tls_certificate(*files, source.private_key_password_);
    }
    const auto& pem = std::get<tls_identity_pem>(source.location_);
    return tls_certificate::load_pem(pem.certificate_chain_, pem.private_key_, source.private_key_password_);
}

[[nodiscard]] http_server_tls_identities::identity load_fixed_tls_identity(const tls_identity_source& source) {
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
        if (!identity.certificate_chain_file_.empty() || !identity.private_key_file_.empty() || !identity.certificate_chain_pem_.empty() || !identity.private_key_pem_.empty() || !identity.private_key_password_.empty() || !config.ocsp_response_.empty() || !config.sni_.empty() || !config.local_addresses_.empty()) {
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

[[nodiscard]] asio::ssl::verify_mode http_server_tls_verify_mode(tls_client_certificate_requirement requirement) noexcept {
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

const http_server_tls_identities::identity& http_server_tls_identities::select(std::string_view server_name, const asio::ip::address* local_address) const noexcept {
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

http_server_listener_definition::tls load_http_server_tls(const tls_config& config) {
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

std::shared_ptr<const http_server_tls_identities> load_http_server_tls_identities(const tls_config& config) {
    return tls_identity_plan(config).load();
}

void ensure_same_http_server_tls_policy(const http_server_listener_definition::tls& current, const tls_config& config) {
    if (config.max_outgoing_records_ != current.max_outgoing_records_) {
        throw std::invalid_argument("replacing TLS identities must keep the listener's outgoing record limit");
    }
    const auto& client = config.client_certificates_;
    const auto& policy = current.client_certificates_;
    const bool same = policy.has_value() ? client.requirement_ == policy->requirement_ && client.ca_file_.has_value() && make_tls_file_name(*client.ca_file_) == policy->ca_file_ : client.requirement_ == tls_client_certificate_requirement::optional && !client.ca_file_.has_value();
    if (!same) {
        throw std::invalid_argument("replacing TLS identities must keep the listener's client certificate policy");
    }
}

void configure_http_server_client_certificates(SSL_CTX& context_value, const std::optional<http_server_listener_definition::tls_client_certificate_policy>& client_certificates) {
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
    SSL_CTX_set_verify(&context_value, static_cast<int>(http_server_tls_verify_mode(client_certificates->requirement_)), nullptr);
}

}  // namespace ruvia::detail
