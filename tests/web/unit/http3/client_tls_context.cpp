#include <filesystem>
#include <fstream>
#include <memory>
#include <memory_resource>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "http3/Http3QuicClientTlsContext.h"
#include "test_harness.h"

namespace {

struct counting_resource final : std::pmr::memory_resource {
    std::size_t allocations{};
    std::size_t deallocations{};
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct TemporaryDirectory final {
    TemporaryDirectory() {
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            path = std::filesystem::temp_directory_path() /
                   ("ruvia-http3-client-tls-" + std::to_string(random()) + "-" +
                       std::to_string(random()));
            std::error_code error;
            if (std::filesystem::create_directory(path, error)) {
                std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                    std::filesystem::perm_options::replace);
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error("create temporary directory", path, error);
            }
        }
        throw std::runtime_error("could not create temporary directory");
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

struct IdentityFiles final {
    explicit IdentityFiles(const std::filesystem::path& directory, std::string_view password = {}) {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keyContext(
            EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY* rawKey = nullptr;
        if (!keyContext || EVP_PKEY_keygen_init(keyContext.get()) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(keyContext.get(), 2048) <= 0 ||
            EVP_PKEY_keygen(keyContext.get(), &rawKey) <= 0) {
            throw std::runtime_error("could not generate test key");
        }
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(rawKey, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
        if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
            X509_gmtime_adj(X509_get_notBefore(certificate.get()), 0) == nullptr ||
            X509_gmtime_adj(X509_get_notAfter(certificate.get()), 86400) == nullptr ||
            X509_set_pubkey(certificate.get(), key.get()) != 1) {
            throw std::runtime_error("could not initialize test certificate");
        }
        const auto name = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
        constexpr char commonName[] = "client.ruvia-test.local";
        X509V3_CTX extensionContext;
        X509V3_set_ctx(&extensionContext, certificate.get(), certificate.get(), nullptr, nullptr, 0);
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
            X509V3_EXT_conf_nid(nullptr, &extensionContext, NID_subject_alt_name,
                const_cast<char*>("DNS:client.ruvia-test.local")),
            X509_EXTENSION_free);
        if (name == nullptr || !san ||
            X509_NAME_add_entry_by_txt(name.get(), "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>(commonName), -1, -1, 0) != 1 ||
            X509_add_ext(certificate.get(), san.get(), -1) != 1 ||
            X509_set_subject_name(certificate.get(), name.get()) != 1 ||
            X509_set_issuer_name(certificate.get(), name.get()) != 1 ||
            X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
            throw std::runtime_error("could not sign test certificate");
        }
        certificateFile = directory / "certificate.pem";
        keyFile = directory / "key.pem";
        std::unique_ptr<BIO, decltype(&BIO_free)> certificateBio(BIO_new_file(
                                                                     certificateFile.string().c_str(), "wb"),
            BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> keyBio(BIO_new_file(keyFile.string().c_str(), "wb"), BIO_free);
        if (!certificateBio || !keyBio || PEM_write_bio_X509(certificateBio.get(), certificate.get()) != 1 ||
            PEM_write_bio_PrivateKey(keyBio.get(), key.get(), password.empty() ? nullptr : EVP_aes_256_cbc(),
                reinterpret_cast<const unsigned char*>(password.data()), static_cast<int>(password.size()), nullptr, nullptr) != 1) {
            throw std::runtime_error("could not write test credentials");
        }
    }
    std::filesystem::path certificateFile;
    std::filesystem::path keyFile;
};

}  // namespace

RUVIA_TEST(http3QuicClientTlsContextConfiguresPeerAndCleansUp) {
    using namespace ruvia::detail;
    TemporaryDirectory directory;
    const IdentityFiles files(directory.path);
    const std::string certificatePath = files.certificateFile.string();
    const std::string keyPath = files.keyFile.string();
    const std::string missingCaPath = (directory.path / "missing-ca.pem").string();
    ClientTransportConfigView config;
    config.tlsPeerVerification = ruvia::TlsPeerVerificationPolicy::kVerify;
    config.caFile = certificatePath;
    config.certificateChainFile = certificatePath;
    config.privateKeyFile = keyPath;
    {
        counting_resource resource;
        http3_quic_client_tls_context context(config, &resource);
        SSL_CTX* const sslContext = context.native_handle();
        RUVIA_CHECK(sslContext != nullptr);
        RUVIA_CHECK(SSL_CTX_get_ssl_method(sslContext) == TLS_method());
        RUVIA_CHECK(SSL_CTX_get_min_proto_version(sslContext) == TLS1_3_VERSION);
        RUVIA_CHECK(SSL_CTX_get_max_proto_version(sslContext) == TLS1_3_VERSION);
        RUVIA_CHECK(SSL_CTX_get_verify_mode(sslContext) == SSL_VERIFY_PEER);
        RUVIA_CHECK(SSL_CTX_check_private_key(sslContext) == 1);

        std::unique_ptr<SSL, decltype(&SSL_free)> dns(SSL_new(sslContext), SSL_free);
        RUVIA_CHECK(dns != nullptr);
        context.prepare(dns.get(), "client.ruvia-test.local.");
        // OpenSSL does not expose the configured client SNI via SSL_get_servername
        // before a peer ClientHello/handshake; prepare() applies the DNS-only SNI.
        // ALPN is likewise an offer and cannot be observed as selected before a handshake.
        RUVIA_CHECK(std::string_view(X509_VERIFY_PARAM_get0_host(SSL_get0_param(dns.get()), 0)) ==
                    "client.ruvia-test.local");

        std::unique_ptr<SSL, decltype(&SSL_free)> ip(SSL_new(sslContext), SSL_free);
        RUVIA_CHECK(ip != nullptr);
        context.prepare(ip.get(), "127.0.0.1");
        RUVIA_CHECK(SSL_get_servername(ip.get(), TLSEXT_NAMETYPE_host_name) == nullptr);
        char* const ipTarget = X509_VERIFY_PARAM_get1_ip_asc(SSL_get0_param(ip.get()));
        RUVIA_CHECK(ipTarget != nullptr);
        RUVIA_CHECK(std::string_view(ipTarget) == "127.0.0.1");
        OPENSSL_free(ipTarget);

        const std::string longHost(48, 'a');
        const std::string host = longHost + ".test";
        std::unique_ptr<SSL, decltype(&SSL_free)> longDns(SSL_new(sslContext), SSL_free);
        RUVIA_CHECK(longDns != nullptr);
        context.prepare(longDns.get(), host);
        RUVIA_CHECK(std::string_view(X509_VERIFY_PARAM_get0_host(SSL_get0_param(longDns.get()), 0)) == host);
        RUVIA_CHECK(resource.allocations != 0);
        RUVIA_CHECK(resource.allocations == resource.deallocations);
    }

    config.caFile = missingCaPath;
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { http3_quic_client_tls_context context(config); }));
    config.caFile = certificatePath;
    config.privateKeyFile = certificatePath;
    RUVIA_CHECK(ruvia::testing::throwsOn([&] { http3_quic_client_tls_context context(config); }));
}

RUVIA_TEST(client_tls_context_shares_identity_loading_and_clears_password_borrows) {
    using namespace ruvia::detail;
    TemporaryDirectory directory;
    const IdentityFiles files(directory.path, "test-password");
    const auto certificate = files.certificateFile.string();
    const auto key = files.keyFile.string();
    for (const auto protocol : {client_tls_protocol::stream, client_tls_protocol::quic}) {
        std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_method()), SSL_CTX_free);
        RUVIA_CHECK(context != nullptr);
        ClientTransportConfigView config;
        config.caFile = certificate;
        config.certificateChainFile = certificate;
        config.privateKeyFile = key;
        config.privateKeyPassword = "test-password";
        configure_client_tls_context(*context, config, protocol);
        RUVIA_CHECK(SSL_CTX_check_private_key(context.get()) == 1);
        RUVIA_CHECK(SSL_CTX_get_verify_mode(context.get()) == SSL_VERIFY_PEER);
        RUVIA_CHECK(SSL_CTX_get_min_proto_version(context.get()) ==
                    (protocol == client_tls_protocol::quic ? TLS1_3_VERSION : TLS1_2_VERSION));
        RUVIA_CHECK(SSL_CTX_get_max_proto_version(context.get()) ==
                    (protocol == client_tls_protocol::quic ? TLS1_3_VERSION : 0));
        RUVIA_CHECK(SSL_CTX_get_default_passwd_cb(context.get()) == nullptr);
        RUVIA_CHECK(SSL_CTX_get_default_passwd_cb_userdata(context.get()) == nullptr);
        config.privateKeyPassword = "incorrect";
        RUVIA_CHECK(ruvia::testing::throwsOn([&] {
            configure_client_tls_context(*context, config, protocol);
        }));
        RUVIA_CHECK(SSL_CTX_get_default_passwd_cb(context.get()) == nullptr);
        RUVIA_CHECK(SSL_CTX_get_default_passwd_cb_userdata(context.get()) == nullptr);
    }
}
