#pragma once

#include <string_view>

#include <openssl/x509.h>

namespace ruvia::detail {

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
    [[nodiscard]] static tls_certificate load(const char* certificate_chain_file,
        const char* private_key_file, std::string_view private_key_password);
    // The same identity from PEM text in memory: the leaf certificate
    // followed by its chain, and the (possibly encrypted) private key. The
    // text is read in place, never copied; the password is borrowed as by
    // load(). Throws on invalid or mismatched PEM.
    [[nodiscard]] static tls_certificate load_pem(std::string_view certificate_chain,
        std::string_view private_key, std::string_view private_key_password);

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
    explicit tls_trust_store(X509_STORE* store) noexcept
        : store_(store) {}

    X509_STORE* store_{};
};

}  // namespace ruvia::detail
