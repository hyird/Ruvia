#pragma once

#include <cstdint>
#include <exception>
#include <string_view>

#include <openssl/types.h>

namespace ruvia::detail {

// The protocol policy of every client and server TLS context. Startup only.
// - TLS 1.2 or newer, no TLS compression.
// - No renegotiation: RFC 9113 §9.2.1 requires it disabled for HTTP/2 over
//   TLS 1.2, and nothing here relies on it for other protocols.
// - TLS 1.2 cipher suites limited to ECDHE key exchange with AEAD ciphers
//   (RFC 9325 §4.1-4.2; none is on RFC 9113 Appendix A's prohibited list),
//   TLS_ECDHE_*_WITH_AES_128_GCM_SHA256 first (RFC 9325 §4.2.1). TLS 1.3
//   suites, all AEAD, keep OpenSSL's defaults.
// - The connection-wide outgoing record limit includes every key epoch and is
//   capped at 2^23. It therefore stays below RFC 9846 §5.5's per-key AES-GCM
//   limit and reaches neither a record sequence nor a KeyUpdate epoch limit.
// Servers additionally choose by their own preference order.
void validate_tls_record_limit(std::uint64_t limit);
void apply_tls_protocol_policy(SSL_CTX& context, std::uint64_t record_limit);

// 原生数据库驱动自己创建 SSL_CTX。作用域只包围一次同步 start/cont/poll 调用，
// 不跨协程挂起：构造通知只安装回调和自有元数据，不驱动未初始化的 SSL 对象。
// 证书回调在实际握手中验证证书链和 SAN，先于任何数据库应用数据。
class native_tls_context_scope final {
public:
    native_tls_context_scope(std::string_view host, std::uint64_t record_limit);
    ~native_tls_context_scope();
    native_tls_context_scope(const native_tls_context_scope&) = delete;
    native_tls_context_scope& operator=(const native_tls_context_scope&) = delete;
    void rethrow_failure() const;

private:
    static void new_context(void* parent, void*, CRYPTO_EX_DATA*, int index, long, void*) noexcept;
    static thread_local native_tls_context_scope* current_;
    std::string_view host_;
    std::uint64_t record_limit_;
    native_tls_context_scope* previous_;
    std::exception_ptr failure_;
};

}  // namespace ruvia::detail
