#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "borrowed_text.h"
#include "http_client_types.h"
#include "http_request.h"

// The OpenSSL side of every client, server and native database driver TLS
// context: protocol policy, handshake results, parsed identities and trust
// stores, and the server certificate snapshot an SNI callback selects from.
namespace ruvia::detail {

// The protocol policy of every client and server TLS context. Startup only.
// - TLS 1.1 or newer, no TLS compression.
// - No renegotiation: RFC 9113 §9.2.1 requires it disabled for HTTP/2 over
//   TLS 1.2, and nothing here relies on it for other protocols.
// - TLS 1.2 cipher suites limited to ECDHE key exchange with AEAD ciphers
//   (RFC 9325 §4.1-4.2; none is on RFC 9113 Appendix A's prohibited list),
//   TLS_ECDHE_*_WITH_AES_128_GCM_SHA256 first (RFC 9325 §4.2.1). TLS 1.3
//   suites, all AEAD, keep OpenSSL's defaults.
// - TLS 1.1 alone additionally permits ECDHE AES-CBC-SHA suites and its
//   mandatory legacy handshake signatures. Certificate and key security
//   checks, and all TLS 1.2/1.3 security checks, retain the original callback.
// - The connection-wide outgoing record limit includes every key epoch and is
//   capped at 2^23. It therefore stays below RFC 9846 §5.5's per-key AES-GCM
//   limit and reaches neither a record sequence nor a KeyUpdate epoch limit.
// Servers additionally choose by their own preference order.
void validate_tls_record_limit(std::uint64_t limit);
void apply_tls_protocol_policy(SSL_CTX& context, std::uint64_t record_limit);

// Native drivers accepting an explicit cipher list use the same preference
// order. The security callback excludes the final four suites in TLS 1.2.
inline constexpr char tls_cipher_suites[] =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305:"
    "ECDHE-ECDSA-AES128-SHA:ECDHE-RSA-AES128-SHA:"
    "ECDHE-ECDSA-AES256-SHA:ECDHE-RSA-AES256-SHA";

// The policy's SSL_CTX message callback, which enforces the outgoing record
// limit. A context that installs its own message callback must forward every
// message to it first.
void count_tls_records(int writing, int version, int content_type, const void* bytes, std::size_t size, SSL* ssl, void* argument) noexcept;

// 原生数据库驱动自己创建 SSL_CTX。作用域只包围一次同步 start/cont/poll 调用，
// 不跨协程挂起：构造通知只安装回调和自有元数据，不驱动未初始化的 SSL 对象。
// HANDSHAKE_START 回调才在已初始化的 SSL 上禁用重协商和压缩。
// 证书回调在实际握手中验证证书链和 SAN，先于任何数据库应用数据。
class native_tls_context_scope final {
public:
    native_tls_context_scope(std::string_view host, std::uint64_t record_limit);
    ~native_tls_context_scope();
    native_tls_context_scope(const native_tls_context_scope&) = delete;
    native_tls_context_scope& operator=(const native_tls_context_scope&) = delete;
    void rethrow_failure() const;

private:
    friend void register_tls_policy_indexes();
    static void new_context(void* parent, void*, CRYPTO_EX_DATA*, int index, long, void*) noexcept;
    static thread_local native_tls_context_scope* current_;
    std::string_view host_;
    std::uint64_t record_limit_;
    native_tls_context_scope* previous_;
    std::exception_ptr failure_;
};

[[nodiscard]] inline bool is_ascii_dns_alnum(unsigned char byte) noexcept {
    return (byte >= '0' && byte <= '9') || (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

[[nodiscard]] inline bool is_ascii_dns_digit(unsigned char byte) noexcept {
    return byte >= '0' && byte <= '9';
}

// An unbracketed IP literal peer, verified against an iPAddress SAN and never
// sent as SNI (RFC 6066 §3); any other host is a DNS name.
[[nodiscard]] inline bool is_client_ip_address(std::string_view host) noexcept {
    return ::ruvia::is_valid_http_ipv4_literal(host) || ::ruvia::is_valid_http_ipv6_literal(host);
}

// Validates an ASCII transport hostname with an optional root dot. This is
// intentionally narrower than a DNS owner name or URI reg-name; the
// four-numeric-label exclusion is lexical and does not parse IPv4 addresses.
[[nodiscard]] inline bool is_valid_dns_host(std::string_view host) noexcept {
    if (host.ends_with('.')) {
        host.remove_suffix(1);
    }
    if (host.empty() || host.ends_with('.') || host.size() > 253) {
        return false;
    }

    std::size_t labels = 0;
    bool saw_dot = false;
    bool all_labels_numeric = true;
    std::size_t label_start = 0;
    while (label_start < host.size()) {
        const auto label_end = host.find('.', label_start);
        const auto end = label_end == std::string_view::npos ? host.size() : label_end;
        const auto label_length = end - label_start;
        if (label_length == 0 || label_length > 63) {
            return false;
        }
        const auto first = static_cast<unsigned char>(host[label_start]);
        const auto last = static_cast<unsigned char>(host[end - 1]);
        if (!is_ascii_dns_alnum(first) || !is_ascii_dns_alnum(last)) {
            return false;
        }

        bool label_numeric = true;
        for (std::size_t i = label_start; i < end; ++i) {
            const auto byte = static_cast<unsigned char>(host[i]);
            if (!is_ascii_dns_alnum(byte) && byte != '-') {
                return false;
            }
            label_numeric = label_numeric && is_ascii_dns_digit(byte);
        }
        all_labels_numeric = all_labels_numeric && label_numeric;
        ++labels;
        if (label_end == std::string_view::npos) {
            break;
        }
        saw_dot = true;
        label_start = end + 1;
    }
    return !(saw_dot && labels == 4 && all_labels_numeric);
}

// What a finished TLS handshake negotiated with the peer: the selected ALPN
// protocol, the offered server_name and the verified peer certificate identity.

// The ALPN protocol the handshake selected; empty when none was negotiated.
[[nodiscard]] inline std::string_view tls_selected_alpn(SSL& ssl) noexcept {
    const unsigned char* selected = nullptr;
    unsigned int size = 0;
    SSL_get0_alpn_selected(&ssl, &selected, &size);
    return {reinterpret_cast<const char*>(selected), size};
}

// The host_name the client offered in server_name (RFC 6066 §3); empty when
// it offered none. The view lives as long as the SSL.
[[nodiscard]] inline std::string_view tls_server_name(SSL& ssl) noexcept {
    const char* name = SSL_get_servername(&ssl, TLSEXT_NAMETYPE_host_name);
    return name == nullptr ? std::string_view{} : std::string_view(name);
}

// Surface certificate presence independently of its Subject: RFC 5280 section
// 4.1.2.6 permits an empty DN with a critical subjectAltName. Conversion failure
// must not turn a verified identity into the "no certificate" alternative.
inline std::optional<std::string_view> extract_tls_client_certificate(SSL* ssl, std::string& out) {
    out.clear();
    const X509* const certificate = SSL_get0_peer_certificate(ssl);
    if (certificate == nullptr) {
        return std::nullopt;
    }
    const X509_NAME* subject = X509_get_subject_name(certificate);
    if (subject == nullptr) {
        throw std::runtime_error("TLS client certificate has no subject");
    }
    // Render the DN in RFC 2253 form through a memory BIO. Unlike
    // X509_NAME_oneline into a fixed 256-byte buffer, this captures the full
    // subject without silent truncation and produces the unambiguous,
    // standard-parseable form recommended for authorization.
    const auto bio = std::unique_ptr<BIO, decltype(&BIO_free)>(BIO_new(BIO_s_mem()), &BIO_free);
    if (bio == nullptr) {
        throw std::runtime_error("TLS client certificate subject buffer could not be allocated");
    }
    if (X509_NAME_print_ex(bio.get(), subject, 0, XN_FLAG_RFC2253) < 0) {
        throw std::runtime_error("TLS client certificate subject could not be rendered");
    }
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio.get(), &data);
    if (length < 0 || (length > 0 && data == nullptr)) {
        throw std::runtime_error("TLS client certificate subject could not be read");
    }
    if (length > 0) {
        out.assign(data, static_cast<std::size_t>(length));
    }
    return std::string_view(out);
}

// The certificate one server TLS connection presented, answering RFC 9110
// §7.4's question whether it is valid for a request's target host. RFC 9525
// §6.3 matching: subjectAltName DNS-ID or IP-ID only (the subject CN is never
// consulted), a wildcard only as a whole left-most label. Matching against the
// certificate, not the SNI name, keeps RFC 9113 §9.1.1 connection reuse
// working for every name the certificate covers.
//
// Lives on the connection's frame after the handshake and is used only on its
// thread. Verdicts are cached for the last few distinct hosts, so a
// connection pays OpenSSL's certificate scan once per host, not per request.
class tls_served_certificate final {
public:
    // `certificate` is the leaf the handshake selected and installed on its
    // SSL (SSL_get_certificate), borrowed for the connection's lifetime;
    // null fails every check closed.
    explicit tls_served_certificate(X509* certificate) noexcept :
        certificate_(certificate) {}

    tls_served_certificate(const tls_served_certificate&) = delete;
    tls_served_certificate& operator=(const tls_served_certificate&) = delete;

    // `origin` must ask for verify_certificate.
    [[nodiscard]] bool covers(const http_target_origin& origin) noexcept;

private:
    // DNS names are at most 253 octets without the trailing dot; a longer
    // host is checked without caching.
    static constexpr std::size_t cached_key_capacity = 253;
    static constexpr std::size_t key_storage_capacity = 256;
    static constexpr std::size_t cache_size = 4;

    struct cached_verdict final {
        std::uint8_t key_offset_{0};
        std::uint8_t key_size_{0};
        http_target_host_kind kind_{http_target_host_kind::dns_name};
        bool covered_{false};
    };

    [[nodiscard]] bool check(const http_target_origin& origin) const noexcept;

    X509* certificate_;
    std::array<char, key_storage_capacity> key_storage_{};
    std::array<cached_verdict, cache_size> cache_{};
    std::uint16_t key_used_{0};
    std::uint8_t cached_{0};
};

// Throws asio::system_error for the failed `operation` with the oldest queued
// OpenSSL error, translated into the system or SSL category. Callers clear the
// queue before the call.
[[noreturn]] void throw_tls_credential_error(const char* operation);
// A certificate, its chain and private key, parsed from PEM once and
// installed into every SSL_CTX or server connection that presents them.
// Copies share the immutable OpenSSL objects through their own reference
// counts, so presenters never re-read or re-parse the source (and cannot
// observe different files if they rotate later).
class tls_certificate final {
public:
    // The one PEM identity loader for files. File names are NUL-terminated
    // UTF-8, which OpenSSL's file BIOs decode (on Windows into _wfopen). The
    // password is only borrowed while the key is decrypted; an empty password
    // uses the noninteractive default. Throws on unreadable, invalid or
    // mismatched files.
    [[nodiscard]] static tls_certificate load(const char* certificate_chain_file, const char* private_key_file, std::string_view private_key_password);
    // The same identity from PEM text in memory: the leaf certificate
    // followed by its chain, and the (possibly encrypted) private key. The
    // text is read in place, never copied; the password is borrowed as by
    // load(). Throws on invalid or mismatched PEM.
    [[nodiscard]] static tls_certificate load_pem(std::string_view certificate_chain, std::string_view private_key, std::string_view private_key_password);

    tls_certificate(const tls_certificate& other);
    tls_certificate(tls_certificate&& other) noexcept;
    tls_certificate& operator=(tls_certificate&& other) noexcept;
    ~tls_certificate();

    // Startup only: the context is not yet shared with any connection.
    void install(SSL_CTX& context) const;
    // Makes this the identity one server connection presents, replacing the
    // whole identity it held; called during its handshake, before the
    // certificate is chosen. The connection takes its own references. False
    // when OpenSSL refuses the identity.
    [[nodiscard]] bool present(SSL& connection) const noexcept;

private:
    // Takes additional references; throws std::bad_alloc when the chain
    // cannot be referenced.
    tls_certificate(X509* certificate, STACK_OF(X509) * chain, EVP_PKEY* private_key);
    // Checks that the scratch context's certificate and key match, then
    // references its identity.
    [[nodiscard]] static tls_certificate adopt_loaded(SSL_CTX& scratch);

    void release() noexcept;

    X509* certificate_{};
    STACK_OF(X509) * chain_ {};
    EVP_PKEY* private_key_{};
};

// A certificate trust store parsed once at startup and shared read-only by
// every SSL_CTX that verifies peers against it.
class tls_trust_store final {
public:
    // Loads a PEM CA bundle; the file name is NUL-terminated UTF-8.
    [[nodiscard]] static tls_trust_store load_file(const char* file);
    // OpenSSL's default verify locations (the system trust store).
    [[nodiscard]] static tls_trust_store load_default();

    tls_trust_store(const tls_trust_store& other);
    tls_trust_store(tls_trust_store&& other) noexcept;
    tls_trust_store& operator=(const tls_trust_store& other);
    tls_trust_store& operator=(tls_trust_store&& other) noexcept;
    ~tls_trust_store();

    // Startup only: replaces the context's trust store with this shared one.
    void install(SSL_CTX& context) const noexcept;

private:
    // Adopts the caller's reference.
    explicit tls_trust_store(X509_STORE* store) noexcept :
        store_(store) {}

    X509_STORE* store_{};
};

// A TLS file name in UTF-8, the encoding OpenSSL's file BIOs decode (on
// Windows into a wide name for _wfopen). Converting through char8_t never
// narrows a native wide path through the ANSI code page.
using tls_file_name = std::u8string;

// The one TLS path validator, shared by servers and clients: a path must not
// be empty, and since OpenSSL and the database drivers consume C strings, it
// must not contain NUL either. Throws std::invalid_argument.
[[nodiscard]] inline tls_file_name make_tls_file_name(const std::filesystem::path& file) {
    if (file.empty()) {
        throw std::invalid_argument("TLS file paths must not be empty");
    }
    auto name = file.u8string();
    if (name.find(u8'\0') != tls_file_name::npos) {
        throw std::invalid_argument("TLS file paths must not contain NUL bytes");
    }
    return name;
}

// The NUL-terminated UTF-8 form handed to OpenSSL and the database drivers.
[[nodiscard]] inline const char* tls_file_name_c_str(const tls_file_name& name RUVIA_LIFETIMEBOUND) noexcept {
    return reinterpret_cast<const char*>(name.c_str());
}
const char* tls_file_name_c_str(tls_file_name&&) = delete;

// One identity's validated file names. The private key password is not kept:
// it is needed only while the key is loaded.
struct tls_identity_files final {
    tls_file_name certificate_chain_file_;
    tls_file_name private_key_file_;
};

[[nodiscard]] inline tls_identity_files make_tls_identity_files(const tls_identity_config& identity) {
    return {
        .certificate_chain_file_ = make_tls_file_name(identity.certificate_chain_file_),
        .private_key_file_ = make_tls_file_name(identity.private_key_file_),
    };
}

[[nodiscard]] inline tls_identity_files copy_tls_identity_files(const tls_identity_files& files) {
    return files;
}

// Reads and parses the files once; throws on unreadable, invalid or mismatched files.
[[nodiscard]] inline tls_certificate load_tls_certificate(const tls_identity_files& files, std::string_view private_key_password) {
    return tls_certificate::load(tls_file_name_c_str(files.certificate_chain_file_), tls_file_name_c_str(files.private_key_file_), private_key_password);
}
[[nodiscard]] inline tls_trust_store load_tls_trust_store(const tls_file_name& ca_file) {
    return tls_trust_store::load_file(tls_file_name_c_str(ca_file));
}

class tls_certificate_snapshot final {
public:
    struct entry_type final {
        tls_certificate_id id_{};
        std::shared_ptr<const tls_certificate> certificate_;
    };

    [[nodiscard]] const tls_certificate* find(tls_certificate_id id) const noexcept {
        const auto it = std::ranges::lower_bound(entries_, id, {}, &entry_type::id_);
        return (it != entries_.end() && it->id_ == id) ? it->certificate_.get() : nullptr;
    }

    [[nodiscard]] bool contains(tls_certificate_id id) const noexcept {
        return find(id) != nullptr;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return entries_.size();
    }

    void insert_or_assign(tls_certificate_id id, std::shared_ptr<const tls_certificate> certificate) {
        const auto it = std::ranges::lower_bound(entries_, id, {}, &entry_type::id_);
        if (it != entries_.end() && it->id_ == id) {
            it->certificate_ = std::move(certificate);
        } else {
            entries_.insert(it, entry_type{id, std::move(certificate)});
        }
    }

    void erase(tls_certificate_id id) noexcept {
        const auto it = std::ranges::lower_bound(entries_, id, {}, &entry_type::id_);
        if (it != entries_.end() && it->id_ == id) {
            entries_.erase(it);
        }
    }

private:
    std::vector<entry_type> entries_;
};

struct tls_certificate_store_access final {
    [[nodiscard]] static std::shared_ptr<const tls_certificate_snapshot> snapshot(const tls_certificate_store& store) noexcept;
};

// Same validated loader used by fixed listener identities and incremental IDs.
[[nodiscard]] tls_certificate load_server_tls_certificate(const tls_identity_config& identity);

}  // namespace ruvia::detail
