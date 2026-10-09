#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "http3/http3_quic_tls_context.h"
#include "test_harness.h"
#include "test_tls_crypto.h"

namespace {

struct temporary_directory final {
    temporary_directory() {
        std::random_device random;
        for (int attempt_value = 0; attempt_value < 100; ++attempt_value) {
            path_ = std::filesystem::temp_directory_path() /
                    ("ruvia-http3-tls-" + std::to_string(random()) + "-" +
                        std::to_string(random()));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                std::filesystem::permissions(path_,
                    std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error("create temporary directory", path_, error);
            }
        }
        throw std::runtime_error("could not create unique temporary directory");
    }

    ~temporary_directory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    std::filesystem::path path_;
};

struct bio_deleter final {
    void operator()(BIO* bio) const noexcept {
        BIO_free(bio);
    }
};
struct key_deleter final {
    void operator()(EVP_PKEY* key) const noexcept {
        EVP_PKEY_free(key);
    }
};
struct certificate_deleter final {
    void operator()(X509* certificate) const noexcept {
        X509_free(certificate);
    }
};

std::string bio_contents(BIO* bio) {
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    if (size <= 0 || data == nullptr) {
        throw std::runtime_error("could not read generated PEM data");
    }
    return {data, static_cast<std::size_t>(size)};
}

void write_pem(const std::filesystem::path& path, const std::string& pem) {
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.write(pem.data(), static_cast<std::streamsize>(pem.size()));
}

struct test_identity_files final {
    test_identity_files(const std::filesystem::path& directory, const char* common_name,
        long serial) {
        using key_type = std::unique_ptr<EVP_PKEY, key_deleter>;
        using certificate_type = std::unique_ptr<X509, certificate_deleter>;
        using bio_type = std::unique_ptr<BIO, bio_deleter>;

        EVP_PKEY_CTX* raw_key_context = EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr);
        if (raw_key_context == nullptr) {
            throw std::runtime_error("could not create RSA key generator");
        }
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> key_context(
            raw_key_context, EVP_PKEY_CTX_free);
        EVP_PKEY* raw_key = nullptr;
        if (EVP_PKEY_keygen_init(key_context.get()) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(key_context.get(), 2048) <= 0 ||
            EVP_PKEY_generate(key_context.get(), &raw_key) <= 0) {
            throw std::runtime_error("could not generate RSA key");
        }
        key_type key(raw_key);

        certificate_type certificate(X509_new_ex(nullptr, nullptr));
        if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial) != 1 ||
            X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) == nullptr ||
            X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400) == nullptr ||
            X509_set_pubkey(certificate.get(), key.get()) != 1) {
            throw std::runtime_error("could not initialize self-signed certificate");
        }
        const auto name = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
        X509V3_CTX extension_context;
        X509V3_set_ctx(&extension_context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
        const std::string san = std::string("DNS:") + common_name;
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san_extension(
            X509V3_EXT_nconf_nid(nullptr, &extension_context, NID_subject_alt_name,
                const_cast<char*>(san.c_str())),
            X509_EXTENSION_free);
        if (!name || X509_NAME_add_entry_by_txt(name.get(), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0) != 1 ||
            X509_add_ext(certificate.get(), san_extension.get(), -1) != 1 ||
            X509_set_subject_name(certificate.get(), name.get()) != 1 ||
            X509_set_issuer_name(certificate.get(), name.get()) != 1 ||
            ruvia::test::sign_tls_certificate(certificate.get(), key.get()) <= 0) {
            throw std::runtime_error("could not sign self-signed certificate");
        }

        bio_type certificate_bio(BIO_new(BIO_s_mem()));
        bio_type key_bio(BIO_new(BIO_s_mem()));
        if (!certificate_bio || !key_bio ||
            PEM_write_bio_X509(certificate_bio.get(), certificate.get()) != 1 ||
            ruvia::test::write_tls_private_key(key_bio.get(), key.get()) !=
                1) {
            throw std::runtime_error("could not encode generated PEM data");
        }
        certificate_file_ = directory / (std::string(common_name) + "-certificate.pem");
        key_file_ = directory / (std::string(common_name) + "-private-key.pem");
        write_pem(certificate_file_, bio_contents(certificate_bio.get()));
        write_pem(key_file_, bio_contents(key_bio.get()));
    }

    std::filesystem::path certificate_file_;
    std::filesystem::path key_file_;
};

void set_identity(ruvia::detail::http_server_listener_definition::tls_identity_type& identity,
    const test_identity_files& files) {
    identity.certificate_chain_file_ = files.certificate_file_.string();
    identity.private_key_file_ = files.key_file_.string();
}

}  // namespace

RUVIA_TEST(http3_quic_tls_context_configures_tls_and_certificate_policies) {
    using namespace ruvia::detail;

    temporary_directory directory;
    const test_identity_files default_files(directory.path_, "default.ruvia-test.local", 1);
    const test_identity_files sni_files(directory.path_, "sni.ruvia-test.local", 2);
    http_server_listener_definition::tls_type tls;
    set_identity(tls.identity_, default_files);
    tls.sni_identities_.emplace_back();
    tls.sni_identities_.back().host_ = "sni.ruvia-test.local";
    set_identity(tls.sni_identities_.back().identity_, sni_files);

    for (const auto requirement : {ruvia::tls_client_certificate_requirement::optional,
             ruvia::tls_client_certificate_requirement::required}) {
        http_server_listener_definition::tls_client_certificate_policy_type policy;
        policy.verify_file_ = default_files.certificate_file_.string();
        policy.requirement_ = requirement;
        tls.client_certificates_ = policy;

        http3_quic_tls_context context(tls, std::pmr::get_default_resource());
        SSL_CTX* const default_context = context.default_context();
        RUVIA_CHECK(default_context != nullptr);
        RUVIA_CHECK(SSL_CTX_get_ssl_method(default_context) == TLS_method());
        RUVIA_CHECK(SSL_CTX_get_min_proto_version(default_context) == TLS1_3_VERSION);
        RUVIA_CHECK(SSL_CTX_get_max_proto_version(default_context) == TLS1_3_VERSION);
        RUVIA_CHECK(SSL_CTX_check_private_key(default_context) == 1);
        RUVIA_CHECK(SSL_CTX_get_verify_mode(default_context) ==
                    (SSL_VERIFY_PEER | (requirement == ruvia::tls_client_certificate_requirement::required
                                               ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT
                                               : 0)));

        X509* const default_certificate = SSL_CTX_get0_certificate(default_context);
        RUVIA_CHECK(default_certificate != nullptr);
        const auto* subject = X509_get_subject_name(default_certificate);
        const int name_index = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
        RUVIA_CHECK(name_index >= 0);
        if (name_index >= 0) {
            const auto* name = X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, name_index));
            const auto default_name = std::string_view(
                reinterpret_cast<const char*>(ASN1_STRING_get0_data(name)),
                static_cast<std::size_t>(ASN1_STRING_length(name)));
            RUVIA_CHECK(default_name == "default.ruvia-test.local");
        }
    }

    http_server_listener_definition::tls_type missing_certificate;
    missing_certificate.identity_.private_key_file_ = default_files.key_file_.string();
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        http3_quic_tls_context context(missing_certificate, std::pmr::get_default_resource());
    }));
    http_server_listener_definition::tls_type missing_key;
    missing_key.identity_.certificate_chain_file_ = default_files.certificate_file_.string();
    RUVIA_CHECK(ruvia::testing::throws_on([&] {
        http3_quic_tls_context context(missing_key, std::pmr::get_default_resource());
    }));
}
