#include "tls/tls_files.h"

#include <stdexcept>

#include "ruvia/core/memory/pmr_object.h"

namespace ruvia::detail {

tls_file_name make_tls_file_name(const std::filesystem::path& file, std::pmr::memory_resource* resource) {
    if (file.empty()) {
        throw std::invalid_argument("TLS file paths must not be empty");
    }
    auto name = file.string<char8_t, std::char_traits<char8_t>, std::pmr::polymorphic_allocator<char8_t>>(
        std::pmr::polymorphic_allocator<char8_t>(pmr_resource_or_default(resource)));
    if (name.find(u8'\0') != tls_file_name::npos) {
        throw std::invalid_argument("TLS file paths must not contain NUL bytes");
    }
    return name;
}

tls_identity_files make_tls_identity_files(const tls_identity_config& identity, std::pmr::memory_resource* resource) {
    return {
        .certificate_chain_file_ = make_tls_file_name(identity.certificate_chain_file_, resource),
        .private_key_file_ = make_tls_file_name(identity.private_key_file_, resource),
    };
}

tls_identity_files copy_tls_identity_files(const tls_identity_files& files, std::pmr::memory_resource* resource) {
    auto* const target = pmr_resource_or_default(resource);
    return {
        .certificate_chain_file_ = tls_file_name(files.certificate_chain_file_, target),
        .private_key_file_ = tls_file_name(files.private_key_file_, target),
    };
}

tls_certificate load_tls_certificate(const tls_identity_files& files, std::string_view private_key_password) {
    return tls_certificate::load(tls_file_name_c_str(files.certificate_chain_file_),
        tls_file_name_c_str(files.private_key_file_), private_key_password);
}

tls_trust_store load_tls_trust_store(const tls_file_name& ca_file) {
    return tls_trust_store::load_file(tls_file_name_c_str(ca_file));
}

}  // namespace ruvia::detail
