#pragma once

#include <algorithm>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <openssl/x509.h>

#include "ruvia/http/borrowed_text.h"
#include "ruvia/web/tls_config.h"

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

namespace ruvia::detail {

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
[[nodiscard]] inline tls_certificate load_tls_certificate(
    const tls_identity_files& files, std::string_view private_key_password) {
    return tls_certificate::load(tls_file_name_c_str(files.certificate_chain_file_),
        tls_file_name_c_str(files.private_key_file_), private_key_password);
}
[[nodiscard]] inline tls_trust_store load_tls_trust_store(const tls_file_name& ca_file) {
    return tls_trust_store::load_file(tls_file_name_c_str(ca_file));
}
}  // namespace ruvia::detail

namespace ruvia::detail {

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
    [[nodiscard]] static std::shared_ptr<const tls_certificate_snapshot> snapshot(
        const tls_certificate_store& store) noexcept;
};

// Same validated loader used by fixed listener identities and incremental IDs.
[[nodiscard]] tls_certificate load_server_tls_certificate(const tls_identity_config& identity);

}  // namespace ruvia::detail
