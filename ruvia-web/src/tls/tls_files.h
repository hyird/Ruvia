#pragma once

#include <filesystem>
#include <memory_resource>
#include <string>
#include <string_view>

#include "ruvia/web/tls_config.h"

#include "tls/tls_credential.h"

namespace ruvia::detail {

// A TLS file name in UTF-8, the encoding OpenSSL's file BIOs decode (on
// Windows into a wide name for _wfopen). Converting through char8_t never
// narrows a native wide path through the ANSI code page.
using tls_file_name = std::pmr::u8string;

// The one TLS path validator, shared by servers and clients: a path must not
// be empty, and since OpenSSL and the database drivers consume C strings, it
// must not contain NUL either. Throws std::invalid_argument.
[[nodiscard]] tls_file_name make_tls_file_name(const std::filesystem::path& file, std::pmr::memory_resource* resource);

// The NUL-terminated UTF-8 form handed to OpenSSL and the database drivers.
[[nodiscard]] inline const char* tls_file_name_c_str(const tls_file_name& name) noexcept {
    return reinterpret_cast<const char*>(name.c_str());
}

// One identity's validated file names. The private key password is not kept:
// it is needed only while the key is loaded.
struct tls_identity_files final {
    tls_file_name certificate_chain_file_;
    tls_file_name private_key_file_;
};

[[nodiscard]] tls_identity_files make_tls_identity_files(
    const tls_identity_config& identity, std::pmr::memory_resource* resource);

// Rebinds validated names to another resource without re-validating them.
[[nodiscard]] tls_identity_files copy_tls_identity_files(
    const tls_identity_files& files, std::pmr::memory_resource* resource);

// Reads and parses the files once; throws on unreadable, invalid or mismatched files.
[[nodiscard]] tls_certificate load_tls_certificate(const tls_identity_files& files, std::string_view private_key_password);
[[nodiscard]] tls_trust_store load_tls_trust_store(const tls_file_name& ca_file);

}  // namespace ruvia::detail
