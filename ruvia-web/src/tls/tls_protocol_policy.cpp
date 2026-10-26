#include "tls/tls_protocol_policy.h"

#include <cstddef>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <utility>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include "ruvia/core/memory/pmr_object.h"

#include "client/client_host.h"

namespace ruvia::detail {
namespace {

struct tls_context_policy final {
    tls_context_policy(std::string_view host, std::uint64_t limit)
        : host_(host, process_resource()),
          record_limit_(limit) {
        if (host_.ends_with('.')) {
            host_.pop_back();
        }
    }
    std::pmr::string host_;
    std::uint64_t record_limit_;
};

struct tls_record_state final {
    std::uint64_t limit_;
    std::uint64_t records_{0};
    bool blocked_{false};
};

void free_context_policy(void*, void* value, CRYPTO_EX_DATA*, int, long, void*) noexcept {
    destroy_pmr_object(static_cast<tls_context_policy*>(value), process_resource());
}

void free_record_state(void*, void* value, CRYPTO_EX_DATA*, int, long, void*) noexcept {
    destroy_pmr_object(static_cast<tls_record_state*>(value), process_resource());
}

int copy_record_state(CRYPTO_EX_DATA*, const CRYPTO_EX_DATA*, void** value, int, long, void*) noexcept {
    try {
        if (*value) {
            *value = construct_pmr_object<tls_record_state>(process_resource(), *static_cast<tls_record_state*>(*value));
        }
        return 1;
    } catch (...) {
        return 0;
    }
}

int context_index = -1;
int record_index = -1;
int bio_index = -1;

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

void count_messages(int writing, int, int content_type, const void*, std::size_t, SSL* ssl, void*) noexcept {
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
            auto owner = make_pmr_object<tls_record_state>(process_resource(), tls_record_state{policy->record_limit_});
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
    SSL_CTX_set_msg_callback(&context, count_messages);
    SSL_CTX_set_msg_callback_arg(&context, nullptr);
    if (!policy->host_.empty()) {
        SSL_CTX_set_cert_verify_callback(&context, verify_native_certificate, policy);
    }
}

}  // namespace

thread_local native_tls_context_scope* native_tls_context_scope::current_ = nullptr;

native_tls_context_scope::native_tls_context_scope(std::string_view host, std::uint64_t record_limit)
    : host_(host),
      record_limit_(record_limit),
      previous_(current_) {
    // 构造通知只安装回调和 ex_data；不查询未初始化的 context 参数或 SSL 状态。
    static const bool initialized = [] {
        context_index = SSL_CTX_get_ex_new_index(0, nullptr, new_context, nullptr, free_context_policy);
        record_index = SSL_get_ex_new_index(0, nullptr, nullptr, copy_record_state, free_record_state);
        bio_index = BIO_get_ex_new_index(0, nullptr, nullptr, nullptr, nullptr);
        if (context_index < 0 || record_index < 0 || bio_index < 0) {
            throw std::runtime_error("failed to register TLS policy storage");
        }
        return true;
    }();
    (void)initialized;
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
        auto owner = make_pmr_object<tls_context_policy>(process_resource(), current_->host_, current_->record_limit_);
        if (CRYPTO_set_ex_data(storage, index, owner.get()) != 1) {
            throw std::bad_alloc();
        }
        install_context_callbacks(*context, owner.release());
    } catch (...) {
        current_->failure_ = std::current_exception();
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
    native_tls_context_scope initialize({}, record_limit);
    auto owner = make_pmr_object<tls_context_policy>(process_resource(), std::string_view{}, record_limit);
    auto* previous = static_cast<tls_context_policy*>(SSL_CTX_get_ex_data(&context, context_index));
    if (SSL_CTX_set_ex_data(&context, context_index, owner.get()) != 1) {
        throw std::bad_alloc();
    }
    auto* policy = owner.release();
    destroy_pmr_object(previous, process_resource());
    install_context_callbacks(context, policy);
    static constexpr char tls12_cipher_suites[] =
        "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
        "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
        "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";
    if (SSL_CTX_set_min_proto_version(&context, TLS1_2_VERSION) != 1 || SSL_CTX_set_max_proto_version(&context, 0) != 1) {
        throw std::runtime_error("failed to configure TLS versions");
    }
    SSL_CTX_set_options(&context, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);
    if (SSL_CTX_set_cipher_list(&context, tls12_cipher_suites) != 1) {
        throw std::runtime_error("failed to configure TLS 1.2 cipher suites");
    }
}

}  // namespace ruvia::detail
