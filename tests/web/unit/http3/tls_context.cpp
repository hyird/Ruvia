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

#include "http3/Http3QuicTlsContext.h"
#include "test_harness.h"

namespace {

struct TemporaryDirectory final {
    TemporaryDirectory() {
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                   ("ruvia-http3-tls-" + std::to_string(random()) + "-" +
                       std::to_string(random()));
            std::error_code error;
            if (std::filesystem::create_directory(path, error)) {
                std::filesystem::permissions(path,
                    std::filesystem::perms::owner_all, std::filesystem::perm_options::replace);
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error("create temporary directory", path, error);
            }
        }
        throw std::runtime_error("could not create unique temporary directory");
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    std::filesystem::path path;
};

struct BioDeleter final {
    void operator()(BIO* bio) const noexcept {
        BIO_free(bio);
    }
};
struct KeyDeleter final {
    void operator()(EVP_PKEY* key) const noexcept {
        EVP_PKEY_free(key);
    }
};
struct CertificateDeleter final {
    void operator()(X509* certificate) const noexcept {
        X509_free(certificate);
    }
};

std::string bioContents(BIO* bio) {
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio, &data);
    if (size <= 0 || data == nullptr) {
        throw std::runtime_error("could not read generated PEM data");
    }
    return {data, static_cast<std::size_t>(size)};
}

void writePem(const std::filesystem::path& path, const std::string& pem) {
    std::ofstream file(path, std::ios::binary);
    file.exceptions(std::ios::badbit | std::ios::failbit);
    file.write(pem.data(), static_cast<std::streamsize>(pem.size()));
}

struct TestIdentityFiles final {
    TestIdentityFiles(const std::filesystem::path& directory, const char* commonName,
        long serial) {
        using Key = std::unique_ptr<EVP_PKEY, KeyDeleter>;
        using Certificate = std::unique_ptr<X509, CertificateDeleter>;
        using Bio = std::unique_ptr<BIO, BioDeleter>;

        EVP_PKEY_CTX* rawKeyContext = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (rawKeyContext == nullptr) {
            throw std::runtime_error("could not create RSA key generator");
        }
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keyContext(
            rawKeyContext, EVP_PKEY_CTX_free);
        EVP_PKEY* rawKey = nullptr;
        if (EVP_PKEY_keygen_init(keyContext.get()) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext.get(), 2048) <= 0 ||
            EVP_PKEY_keygen(keyContext.get(), &rawKey) <= 0) {
            throw std::runtime_error("could not generate RSA key");
        }
        Key key(rawKey);

        Certificate certificate(X509_new());
        if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), serial) != 1 ||
            X509_gmtime_adj(X509_get_notBefore(certificate.get()), 0) == nullptr ||
            X509_gmtime_adj(X509_get_notAfter(certificate.get()), 86400) == nullptr ||
            X509_set_pubkey(certificate.get(), key.get()) != 1) {
            throw std::runtime_error("could not initialize self-signed certificate");
        }
        X509_NAME* name = X509_get_subject_name(certificate.get());
        X509V3_CTX extensionContext;
        X509V3_set_ctx(&extensionContext, certificate.get(), certificate.get(), nullptr, nullptr, 0);
        const std::string san = std::string("DNS:") + commonName;
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> sanExtension(
            X509V3_EXT_conf_nid(nullptr, &extensionContext, NID_subject_alt_name,
                const_cast<char*>(san.c_str())),
            X509_EXTENSION_free);
        if (X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>(commonName), -1, -1, 0) != 1 ||
            X509_add_ext(certificate.get(), sanExtension.get(), -1) != 1 ||
            X509_set_issuer_name(certificate.get(), name) != 1 ||
            X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
            throw std::runtime_error("could not sign self-signed certificate");
        }

        Bio certificateBio(BIO_new(BIO_s_mem()));
        Bio keyBio(BIO_new(BIO_s_mem()));
        if (!certificateBio || !keyBio ||
            PEM_write_bio_X509(certificateBio.get(), certificate.get()) != 1 ||
            PEM_write_bio_PrivateKey(keyBio.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr) !=
                1) {
            throw std::runtime_error("could not encode generated PEM data");
        }
        certificateFile = directory / (std::string(commonName) + "-certificate.pem");
        keyFile = directory / (std::string(commonName) + "-private-key.pem");
        writePem(certificateFile, bioContents(certificateBio.get()));
        writePem(keyFile, bioContents(keyBio.get()));
    }

    std::filesystem::path certificateFile;
    std::filesystem::path keyFile;
};

void setIdentity(ruvia::detail::HttpServerListenerDefinition::TlsIdentity& identity,
    const TestIdentityFiles& files) {
    identity.certificateChainFile = files.certificateFile.string();
    identity.privateKeyFile = files.keyFile.string();
}

}  // namespace

RUVIA_TEST(http3QuicTlsContextConfiguresTlsAndCertificatePolicies) {
    using namespace ruvia::detail;

    TemporaryDirectory directory;
    const TestIdentityFiles defaultFiles(directory.path, "default.ruvia-test.local", 1);
    const TestIdentityFiles sniFiles(directory.path, "sni.ruvia-test.local", 2);
    HttpServerListenerDefinition::Tls tls;
    setIdentity(tls.identity, defaultFiles);
    tls.sniIdentities.emplace_back();
    tls.sniIdentities.back().host = "sni.ruvia-test.local";
    setIdentity(tls.sniIdentities.back().identity, sniFiles);

    for (const auto requirement : {ruvia::TlsClientCertificateRequirement::kOptional,
             ruvia::TlsClientCertificateRequirement::kRequired}) {
        HttpServerListenerDefinition::TlsClientCertificatePolicy policy;
        policy.verifyFile = defaultFiles.certificateFile.string();
        policy.requirement = requirement;
        tls.clientCertificates = policy;

        http3_quic_tls_context context(tls, std::pmr::get_default_resource());
        SSL_CTX* const default_context = context.default_context();
        RUVIA_CHECK(default_context != nullptr);
        RUVIA_CHECK(SSL_CTX_get_ssl_method(default_context) == TLS_method());
        RUVIA_CHECK(SSL_CTX_get_min_proto_version(default_context) == TLS1_3_VERSION);
        RUVIA_CHECK(SSL_CTX_get_max_proto_version(default_context) == TLS1_3_VERSION);
        RUVIA_CHECK(SSL_CTX_check_private_key(default_context) == 1);
        RUVIA_CHECK(SSL_CTX_get_verify_mode(default_context) ==
                    (SSL_VERIFY_PEER | (requirement == ruvia::TlsClientCertificateRequirement::kRequired
                                               ? SSL_VERIFY_FAIL_IF_NO_PEER_CERT
                                               : 0)));

        X509* const default_certificate = SSL_CTX_get0_certificate(default_context);
        RUVIA_CHECK(default_certificate != nullptr);
        char default_name[128]{};
        RUVIA_CHECK(X509_NAME_get_text_by_NID(X509_get_subject_name(default_certificate),
                        NID_commonName, default_name, static_cast<int>(sizeof(default_name))) > 0);
        RUVIA_CHECK(std::string_view(default_name) == "default.ruvia-test.local");
    }

    HttpServerListenerDefinition::Tls missingCertificate;
    missingCertificate.identity.privateKeyFile = defaultFiles.keyFile.string();
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        http3_quic_tls_context context(missingCertificate, std::pmr::get_default_resource());
    }));
    HttpServerListenerDefinition::Tls missingKey;
    missingKey.identity.certificateChainFile = defaultFiles.certificateFile.string();
    RUVIA_CHECK(ruvia::testing::throwsOn([&] {
        http3_quic_tls_context context(missingKey, std::pmr::get_default_resource());
    }));
}
