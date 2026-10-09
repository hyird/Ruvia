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

#include "http3/http3_quic_client_tls_context.h"
#include "test_harness.h"
#include "test_tls_crypto.h"

namespace {

struct counting_resource final : std::pmr::memory_resource {
    std::size_t allocations_{};
    std::size_t deallocations_{};
    void* do_allocate(std::size_t size, std::size_t alignment) override {
        ++allocations_;
        return std::pmr::new_delete_resource()->allocate(size, alignment);
    }
    void do_deallocate(void* pointer, std::size_t size, std::size_t alignment) override {
        ++deallocations_;
        std::pmr::new_delete_resource()->deallocate(pointer, size, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

struct temporary_directory final {
    temporary_directory() {
        std::random_device random;
        for (int attempt_value = 0; attempt_value < 100; ++attempt_value) {
            path_ = std::filesystem::temp_directory_path() /
                    ("ruvia-http3-client-tls-" + std::to_string(random()) + "-" +
                        std::to_string(random()));
            std::error_code error;
            if (std::filesystem::create_directory(path_, error)) {
                std::filesystem::permissions(path_, std::filesystem::perms::owner_all,
                    std::filesystem::perm_options::replace);
                return;
            }
            if (error && error != std::errc::file_exists) {
                throw std::filesystem::filesystem_error("create temporary directory", path_, error);
            }
        }
        throw std::runtime_error("could not create temporary directory");
    }
    ~temporary_directory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }
    std::filesystem::path path_;
};

struct identity_files final {
    explicit identity_files(const std::filesystem::path& directory, std::string_view password = {}) {
        std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> key_context(
            EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr), EVP_PKEY_CTX_free);
        EVP_PKEY* raw_key = nullptr;
        if (!key_context || EVP_PKEY_keygen_init(key_context.get()) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(key_context.get(), 2048) <= 0 ||
            EVP_PKEY_generate(key_context.get(), &raw_key) <= 0) {
            throw std::runtime_error("could not generate test key");
        }
        std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
        std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new_ex(nullptr, nullptr), X509_free);
        if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
            ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
            X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) == nullptr ||
            X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400) == nullptr ||
            X509_set_pubkey(certificate.get(), key.get()) != 1) {
            throw std::runtime_error("could not initialize test certificate");
        }
        const auto name = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
        constexpr char common_name[] = "client.ruvia-test.local";
        X509V3_CTX extension_context;
        X509V3_set_ctx(&extension_context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
        std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
            X509V3_EXT_nconf_nid(nullptr, &extension_context, NID_subject_alt_name,
                const_cast<char*>("DNS:client.ruvia-test.local")),
            X509_EXTENSION_free);
        if (name == nullptr || !san ||
            X509_NAME_add_entry_by_txt(name.get(), "CN", MBSTRING_ASC,
                reinterpret_cast<const unsigned char*>(common_name), -1, -1, 0) != 1 ||
            X509_add_ext(certificate.get(), san.get(), -1) != 1 ||
            X509_set_subject_name(certificate.get(), name.get()) != 1 ||
            X509_set_issuer_name(certificate.get(), name.get()) != 1 ||
            ruvia::test::sign_tls_certificate(certificate.get(), key.get()) <= 0) {
            throw std::runtime_error("could not sign test certificate");
        }
        certificate_file_ = directory / "certificate.pem";
        key_file_ = directory / "key.pem";
        std::unique_ptr<BIO, decltype(&BIO_free)> certificate_bio(BIO_new_file(
                                                                      certificate_file_.string().c_str(), "wb"),
            BIO_free);
        std::unique_ptr<BIO, decltype(&BIO_free)> key_bio(BIO_new_file(key_file_.string().c_str(), "wb"), BIO_free);
        if (!certificate_bio || !key_bio || PEM_write_bio_X509(certificate_bio.get(), certificate.get()) != 1 ||
            ruvia::test::write_tls_private_key(key_bio.get(), key.get(), password, !password.empty()) != 1) {
            throw std::runtime_error("could not write test credentials");
        }
    }
    std::filesystem::path certificate_file_;
    std::filesystem::path key_file_;
};

}  // namespace

RUVIA_TEST(http3_quic_client_tls_context_configures_peer_and_cleans_up) {
    using namespace ruvia::detail;
    temporary_directory directory;
    const identity_files files(directory.path_);
    const std::string certificate_path = files.certificate_file_.string();
    const std::string key_path = files.key_file_.string();
    const std::string missing_ca_path = (directory.path_ / "missing-ca.pem").string();
    client_transport_config_view config;
    config.tls_peer_verification_ = ruvia::tls_peer_verification_policy::verify;
    config.ca_file_ = certificate_path;
    config.certificate_chain_file_ = certificate_path;
    config.private_key_file_ = key_path;
    {
        counting_resource resource;
        http3_quic_client_tls_context context_value(config, &resource);
        SSL_CTX* const ssl_context = context_value.native_handle();
        RUVIA_CHECK(ssl_context != nullptr);
        RUVIA_CHECK(SSL_CTX_get_ssl_method(ssl_context) == TLS_method());
        RUVIA_CHECK(SSL_CTX_get_min_proto_version(ssl_context) == TLS1_3_VERSION);
        RUVIA_CHECK(SSL_CTX_get_max_proto_version(ssl_context) == TLS1_3_VERSION);
        RUVIA_CHECK(SSL_CTX_get_verify_mode(ssl_context) == SSL_VERIFY_PEER);
        RUVIA_CHECK(SSL_CTX_check_private_key(ssl_context) == 1);

        std::unique_ptr<SSL, decltype(&SSL_free)> dns(SSL_new(ssl_context), SSL_free);
        RUVIA_CHECK(dns != nullptr);
        context_value.prepare(dns.get(), "client.ruvia-test.local.");
        // OpenSSL does not expose the configured client SNI via SSL_get_servername
        // before a peer ClientHello/handshake; prepare() applies the DNS-only SNI.
        // ALPN is likewise an offer and cannot be observed as selected before a handshake.
        RUVIA_CHECK(std::string_view(X509_VERIFY_PARAM_get0_host(SSL_get0_param(dns.get()), 0)) ==
                    "client.ruvia-test.local");

        std::unique_ptr<SSL, decltype(&SSL_free)> ip(SSL_new(ssl_context), SSL_free);
        RUVIA_CHECK(ip != nullptr);
        context_value.prepare(ip.get(), "127.0.0.1");
        RUVIA_CHECK(SSL_get_servername(ip.get(), TLSEXT_NAMETYPE_host_name) == nullptr);
        char* const ip_target = X509_VERIFY_PARAM_get1_ip_asc(SSL_get0_param(ip.get()));
        RUVIA_CHECK(ip_target != nullptr);
        RUVIA_CHECK(std::string_view(ip_target) == "127.0.0.1");
        OPENSSL_free(ip_target);

        const std::string long_host(48, 'a');
        const std::string host = long_host + ".test";
        std::unique_ptr<SSL, decltype(&SSL_free)> long_dns(SSL_new(ssl_context), SSL_free);
        RUVIA_CHECK(long_dns != nullptr);
        context_value.prepare(long_dns.get(), host);
        RUVIA_CHECK(std::string_view(X509_VERIFY_PARAM_get0_host(SSL_get0_param(long_dns.get()), 0)) == host);
        RUVIA_CHECK(resource.allocations_ != 0);
        RUVIA_CHECK(resource.allocations_ == resource.deallocations_);
    }

    config.ca_file_ = missing_ca_path;
    RUVIA_CHECK(ruvia::testing::throws_on([&] { http3_quic_client_tls_context context_value(config); }));
    config.ca_file_ = certificate_path;
    config.private_key_file_ = certificate_path;
    RUVIA_CHECK(ruvia::testing::throws_on([&] { http3_quic_client_tls_context context_value(config); }));
}

RUVIA_TEST(client_tls_context_shares_identity_loading_and_clears_password_borrows) {
    using namespace ruvia::detail;
    temporary_directory directory;
    const identity_files files(directory.path_, "test-password");
    const auto certificate = files.certificate_file_.string();
    const auto key = files.key_file_.string();
    for (const auto protocol : {client_tls_protocol::stream, client_tls_protocol::quic}) {
        std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context(SSL_CTX_new(TLS_method()), SSL_CTX_free);
        RUVIA_CHECK(context != nullptr);
        client_transport_config_view config;
        config.ca_file_ = certificate;
        config.certificate_chain_file_ = certificate;
        config.private_key_file_ = key;
        config.private_key_password_ = "test-password";
        configure_client_tls_context(*context, config, protocol);
        RUVIA_CHECK(SSL_CTX_check_private_key(context.get()) == 1);
        RUVIA_CHECK(SSL_CTX_get_verify_mode(context.get()) == SSL_VERIFY_PEER);
        RUVIA_CHECK(SSL_CTX_get_min_proto_version(context.get()) ==
                    (protocol == client_tls_protocol::quic ? TLS1_3_VERSION : TLS1_2_VERSION));
        RUVIA_CHECK(SSL_CTX_get_max_proto_version(context.get()) ==
                    (protocol == client_tls_protocol::quic ? TLS1_3_VERSION : 0));
        RUVIA_CHECK(SSL_CTX_get_default_passwd_cb(context.get()) == nullptr);
        RUVIA_CHECK(SSL_CTX_get_default_passwd_cb_userdata(context.get()) == nullptr);
        config.private_key_password_ = "incorrect";
        RUVIA_CHECK(ruvia::testing::throws_on([&] {
            configure_client_tls_context(*context, config, protocol);
        }));
        RUVIA_CHECK(SSL_CTX_get_default_passwd_cb(context.get()) == nullptr);
        RUVIA_CHECK(SSL_CTX_get_default_passwd_cb_userdata(context.get()) == nullptr);
    }
}
