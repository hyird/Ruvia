#include "tls/tls_protocol_policy.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "ruvia/http/http_ascii.h"

#include "client/client_tls.h"
#include "tls/tls_served_certificate.h"
namespace ruvia::detail {
namespace {

using security_callback = int (*)(const SSL*, const SSL_CTX*, int, int, int, void*, void*);

int enforce_security(const SSL*, const SSL_CTX*, int, int, int, void*, void*) noexcept;
void guard_native_handshake(const SSL*, int, int) noexcept;

struct tls_context_policy final {
    tls_context_policy(SSL_CTX& context, std::string_view host, std::uint64_t limit)
        : host_(host),
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

int enforce_security(const SSL* ssl, const SSL_CTX* context, int operation, int bits, int nid,
    void* other, void* opaque) noexcept {
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
    if (operation == SSL_SECOP_CIPHER_SUPPORTED || operation == SSL_SECOP_CIPHER_SHARED ||
        operation == SSL_SECOP_CIPHER_CHECK) {
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
    if (ssl && SSL_version(ssl) == TLS1_1_VERSION &&
        (nid == NID_md5_sha1 || nid == NID_sha1) &&
        (operation == SSL_SECOP_SIGALG_SUPPORTED || operation == SSL_SECOP_SIGALG_SHARED ||
            operation == SSL_SECOP_SIGALG_CHECK || operation == SSL_SECOP_SIGALG_MASK)) {
        // These are handshake signature operations only. In particular,
        // SSL_SECOP_CA_MD, EE_KEY and CA_KEY are never exempted.
        return 1;
    }
    return policy.original_security_ &&
           policy.original_security_(ssl, context, operation, bits, nid, other, policy.original_security_data_);
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
    const auto* policy = static_cast<const tls_context_policy*>(
        SSL_CTX_get_ex_data(SSL_get_SSL_CTX(ssl), context_index));
    if (policy && policy->original_info_) {
        policy->original_info_(ssl, where, result);
    }
}

long deny_output(BIO* bio, int operation, const char*, std::size_t, int, long, int result,
    std::size_t* processed) noexcept {
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

long guard_output(BIO* bio, int operation, const char* bytes, std::size_t size, int arg,
    long larg, int result, std::size_t* processed) noexcept {
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
    const auto matched = ip ? X509_check_ip_asc(leaf, policy.host_.c_str(), 0)
                            : X509_check_host(leaf, policy.host_.c_str(), policy.host_.size(),
                                  X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr);
    if (matched == 1) {
        return 1;
    }
    X509_STORE_CTX_set_error(store, matched == 0
                                        ? (ip ? X509_V_ERR_IP_ADDRESS_MISMATCH : X509_V_ERR_HOSTNAME_MISMATCH)
                                        : X509_V_ERR_APPLICATION_VERIFICATION);
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
        context_index = SSL_CTX_get_ex_new_index(
            0, nullptr, native_tls_context_scope::new_context, nullptr, free_context_policy);
        record_index = SSL_get_ex_new_index(0, nullptr, nullptr, copy_record_state, free_record_state);
        bio_index = BIO_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
        if (context_index < 0 || record_index < 0 || bio_index < 0) {
            throw std::runtime_error("failed to register TLS policy storage");
        }
        return true;
    }();
    (void)registered;
}

native_tls_context_scope::native_tls_context_scope(std::string_view host, std::uint64_t record_limit)
    : host_(host),
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

void native_tls_context_scope::new_context(void* parent, void*, CRYPTO_EX_DATA* storage, int index,
    long, void*) noexcept {
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
        return X509_check_host(certificate_, host.data(), host.size(),
                   X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS | X509_CHECK_FLAG_NEVER_CHECK_SUBJECT, nullptr) == 1;
    }
    const auto address = origin.address();
    return X509_check_ip(certificate_, address.data(), address.size(), 0) == 1;
}

}  // namespace ruvia::detail
