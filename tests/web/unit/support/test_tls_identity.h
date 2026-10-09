#pragma once

#include <filesystem>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>

#include <asio/ssl/context.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "test_tls_crypto.h"

namespace ruvia::test {

class tls_identity final {
public:
    explicit tls_identity(const char* name)
        : context(asio::ssl::context::tls_server) {
        std::random_device random;
        for (int attempt = 0; attempt < 100; ++attempt) {
            directory_ = std::filesystem::temp_directory_path() / ("ruvia-client-tls-" + std::to_string(random()) + "-" + std::to_string(random()));
            if (std::filesystem::create_directory(directory_)) {
                break;
            }
            directory_.clear();
        }
        if (directory_.empty()) {
            throw std::runtime_error("cannot create TLS test directory");
        }
        try {
            std::filesystem::permissions(directory_, std::filesystem::perms::owner_all);
            std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(EVP_PKEY_Q_keygen(nullptr, nullptr, "RSA", std::size_t{2048}), EVP_PKEY_free);
            std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new_ex(nullptr, nullptr), X509_free);
            if (!key || !certificate || X509_set_version(certificate.get(), 2) != 1 ||
                ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
                !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) ||
                !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 86400) ||
                X509_set_pubkey(certificate.get(), key.get()) != 1) {
                throw std::runtime_error("cannot generate TLS test identity");
            }
            const auto subject = std::unique_ptr<X509_NAME, decltype(&X509_NAME_free)>(X509_NAME_new(), X509_NAME_free);
            if (!subject || X509_NAME_add_entry_by_txt(subject.get(), "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>(name), -1, -1, 0) != 1 ||
                X509_set_subject_name(certificate.get(), subject.get()) != 1 ||
                X509_set_issuer_name(certificate.get(), subject.get()) != 1) {
                throw std::runtime_error("cannot name TLS test identity");
            }
            X509V3_CTX extension_context{};
            X509V3_set_ctx(&extension_context, certificate.get(), certificate.get(), nullptr, nullptr, 0);
            const auto san = std::string("DNS:") + name;
            std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> extension(
                X509V3_EXT_nconf_nid(nullptr, &extension_context, NID_subject_alt_name, san.c_str()), X509_EXTENSION_free);
            if (!extension || X509_add_ext(certificate.get(), extension.get(), -1) != 1 ||
                ruvia::test::sign_tls_certificate(certificate.get(), key.get()) <= 0 ||
                SSL_CTX_use_certificate(context.native_handle(), certificate.get()) != 1 ||
                SSL_CTX_use_PrivateKey(context.native_handle(), key.get()) != 1) {
                throw std::runtime_error("cannot sign TLS test identity");
            }
            ca_file = directory_ / "ca.pem";
            std::unique_ptr<BIO, decltype(&BIO_free)> output(BIO_new_file(ca_file.string().c_str(), "w"), BIO_free);
            if (!output || PEM_write_bio_X509(output.get(), certificate.get()) != 1) {
                throw std::runtime_error("cannot write TLS test certificate");
            }
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove_all(directory_, ignored);
            throw;
        }
    }
    ~tls_identity() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }
    asio::ssl::context context;
    std::filesystem::path ca_file;

private:
    std::filesystem::path directory_;
};

}  // namespace ruvia::test
