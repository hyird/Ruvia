#pragma once

#include <limits>
#include <memory>
#include <string_view>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

namespace ruvia::test {

inline int sign_tls_certificate(X509* certificate, EVP_PKEY* key) {
    const auto context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestSignInit_ex(context.get(), nullptr, "SHA256", nullptr, nullptr,
                        key, nullptr) != 1) {
        return 0;
    }
    return X509_sign_ctx(certificate, context.get());
}

inline int write_tls_private_key(BIO* output, const EVP_PKEY* key,
    std::string_view password = {}, bool encrypted = false) {
    if (password.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return 0;
    }
    const auto cipher = std::unique_ptr<EVP_CIPHER, decltype(&EVP_CIPHER_free)>(
        encrypted ? EVP_CIPHER_fetch(nullptr, "AES-256-CBC", nullptr) : nullptr, EVP_CIPHER_free);
    if (encrypted && !cipher) {
        return 0;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(password.empty() ? "" : password.data());
    return PEM_write_bio_PrivateKey_ex(output, key, cipher.get(), encrypted ? bytes : nullptr,
        static_cast<int>(password.size()), nullptr, nullptr, nullptr, nullptr);
}

}  // namespace ruvia::test
