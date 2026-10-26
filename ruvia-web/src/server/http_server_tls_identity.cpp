#include "server/http_server_tls_identity.h"

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <asio/ssl/verify_mode.hpp>
#include <openssl/err.h>
#include <openssl/ssl.h>

#include "ruvia/core/memory/pmr_object.h"
#include "ruvia/http/http_ascii.h"

#include "tls/tls_credential.h"
#include "tls/tls_file_paths.h"
#include "tls/tls_host.h"
#include "tls/tls_protocol_policy.h"

namespace ruvia::detail {
namespace {

[[nodiscard]] asio::ssl::verify_mode http_server_tls_verify_mode(
    tls_client_certificate_requirement requirement) noexcept {
    auto mode = asio::ssl::verify_peer;
    if (requirement == tls_client_certificate_requirement::required) {
        mode |= asio::ssl::verify_fail_if_no_peer_cert;
    }
    return mode;
}

// A TLS file name in UTF-8, the encoding OpenSSL's file BIOs decode (on
// Windows into a wide name for _wfopen). Converting through char8_t never
// narrows a native wide path through the ANSI code page.
using tls_file_name = std::pmr::u8string;

[[nodiscard]] tls_file_name make_tls_file_name(
    const std::filesystem::path& file, std::pmr::memory_resource* resource) {
    auto name = file.string<char8_t, std::char_traits<char8_t>, std::pmr::polymorphic_allocator<char8_t>>(
        std::pmr::polymorphic_allocator<char8_t>(resource));
    validate_tls_file_paths({std::string_view(reinterpret_cast<const char*>(name.data()), name.size())});
    return name;
}

[[nodiscard]] const char* open_ssl_file_name(const tls_file_name& name) noexcept {
    return reinterpret_cast<const char*>(name.c_str());
}

// One identity's validated file names; the password stays borrowed from the
// caller's configuration.
struct tls_identity_files final {
    tls_file_name certificate_chain_file_;
    tls_file_name private_key_file_;
    std::string_view private_key_password_;
};

[[nodiscard]] tls_identity_files make_tls_identity_files(const std::filesystem::path& certificate_chain_file,
    const std::filesystem::path& private_key_file, std::string_view private_key_password,
    std::pmr::memory_resource* resource) {
    if (certificate_chain_file.empty() || private_key_file.empty()) {
        throw std::invalid_argument("TLS certificate chain and private key files must not be empty");
    }
    return {
        .certificate_chain_file_ = make_tls_file_name(certificate_chain_file, resource),
        .private_key_file_ = make_tls_file_name(private_key_file, resource),
        .private_key_password_ = private_key_password,
    };
}

[[nodiscard]] tls_certificate load_tls_certificate(const tls_identity_files& files) {
    return tls_certificate::load(open_ssl_file_name(files.certificate_chain_file_),
        open_ssl_file_name(files.private_key_file_), files.private_key_password_);
}

[[nodiscard]] tls_trust_store load_tls_client_ca(const tls_file_name& verify_file) {
    return tls_trust_store::load_file(open_ssl_file_name(verify_file));
}

}  // namespace

http_server_listener_definition::tls load_http_server_tls(
    const tls_config& config, std::pmr::memory_resource* resource) {
    auto* const target_resource = pmr_resource_or_default(resource);
    validate_tls_record_limit(config.max_outgoing_records_);
    // Every setting is validated before any file is read.
    const auto primary = make_tls_identity_files(
        config.certificate_chain_file_, config.private_key_file_, config.private_key_password_, target_resource);
    const auto& client = config.client_certificates_;
    std::optional<tls_file_name> verify_file;
    if (client.verify_file_.has_value() || client.requirement_ != tls_client_certificate_requirement::optional) {
        switch (client.requirement_) {
            case tls_client_certificate_requirement::optional:
            case tls_client_certificate_requirement::required:
                break;
            default:
                throw std::invalid_argument("TLS client certificate requirement is invalid");
        }
        if (!client.verify_file_.has_value() || client.verify_file_->empty()) {
            throw std::invalid_argument("TLS client certificate CA bundle must not be empty");
        }
        verify_file.emplace(make_tls_file_name(*client.verify_file_, target_resource));
    }
    std::pmr::vector<tls_identity_files> sni_files(target_resource);
    sni_files.reserve(config.sni_.size());
    for (std::size_t i = 0; i < config.sni_.size(); ++i) {
        const auto& sni = config.sni_[i];
        ensure_sni_host(sni.host_);
        sni_files.push_back(make_tls_identity_files(
            sni.certificate_chain_file_, sni.private_key_file_, sni.private_key_password_, target_resource));
        for (std::size_t j = 0; j < i; ++j) {
            if (http_ascii_equals_ignore_case(config.sni_[j].host_, sni.host_)) {
                throw std::invalid_argument("SNI hosts must be unique");
            }
        }
    }

    http_server_listener_definition::tls tls(load_tls_certificate(primary), target_resource);
    tls.max_outgoing_records_ = config.max_outgoing_records_;
    if (verify_file.has_value()) {
        tls.client_certificates_.emplace(http_server_listener_definition::tls_client_certificate_policy{
            .requirement_ = client.requirement_,
            .client_ca_ = load_tls_client_ca(*verify_file),
        });
    }
    tls.sni_identities_.reserve(config.sni_.size());
    for (std::size_t i = 0; i < config.sni_.size(); ++i) {
        tls.sni_identities_.emplace_back(config.sni_[i].host_, load_tls_certificate(sni_files[i]), target_resource);
    }
    return tls;
}

void configure_http_server_tls_identity(SSL_CTX* context_value, const tls_certificate& certificate,
    const std::optional<http_server_listener_definition::tls_client_certificate_policy>&
        client_certificates) {
    if (context_value == nullptr) {
        throw std::invalid_argument("TLS context must not be null");
    }
    certificate.install(*context_value);

    if (!client_certificates.has_value()) {
        return;
    }
    client_certificates->client_ca_.install(*context_value);
    // A server that verifies peers without a session ID context turns every
    // resumption attempt into a fatal handshake error (SSL_CTX_set_session_id_context(3)).
    static constexpr unsigned char session_id_context[] = {'r', 'u', 'v', 'i', 'a'};
    ::ERR_clear_error();
    if (::SSL_CTX_set_session_id_context(
            context_value, session_id_context, sizeof(session_id_context)) != 1) {
        throw_tls_credential_error("set_session_id_context");
    }
    SSL_CTX_set_verify(context_value,
        static_cast<int>(http_server_tls_verify_mode(client_certificates->requirement_)), nullptr);
}

}  // namespace ruvia::detail
